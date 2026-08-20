#include <rx_material/material_system.h>

#include <rx_core/debug_checks.h>
#include <rx_core/handle.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_material/draw_data.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_rhi_vk/texture.h>
#include <rx_rhi_vk/upload.h>

#include <slang-com-ptr.h>
#include <slang.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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
//
// Task 7 additionally makes this class the SOLE Slang reflection authority
// for a loaded material: reflectMaterialLayout() below now also walks
// gParams's own fields (name/kind/offset/size -- rx_material::
// MaterialParamInfo, instance.h) from the SAME already-linked
// slang::IComponentType every material's real SPIR-V comes from. Task 6's
// api_impl.cpp used to run a SECOND, throwaway reflection-only Slang
// session for this (see that file's OLD header comment, since rewritten);
// that duplicate session is gone -- api_impl.cpp now calls
// MaterialSystem::materialParams()/paramBlockSize() directly.
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
// layout across samples). [Phase 4 Task 16, D8] GREW BY ONE FIELD:
// `tangent` (float4, w = handedness) -- position/normal/tangent/uv,
// tightly packed, one interleaved binding, byte-identical to D8's pooled
// vertex format (`rx::asset::PoolVertex`, geometry_pool.h -- see that
// header's own `static_assert(sizeof(PoolVertex) == 48)`, matched here by
// construction since both list the same four fields in the same order).
// Matches forward_entry.slang's own
// vertexMain(float3 position, float3 normal, float4 tangent, float2 uv,
// uint instanceId) parameter list and the attribute locations Slang's
// SPIR-V backend assigns them (verified directly via spirv-dis before
// writing this for the original three-field layout -- see task-5-report.md;
// locations remain declaration-order, 0/1/2/3, for the same reason with a
// fourth field appended).
struct MaterialVertexLayout {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
};
static_assert(sizeof(MaterialVertexLayout) == 48,
              "MaterialVertexLayout must stay exactly 48 bytes -- must match rx::asset::PoolVertex (D8) byte-for-byte");

struct VertexInputState {
    VkVertexInputBindingDescription binding{};
    std::array<VkVertexInputAttributeDescription, 4> attributes{};
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
    state.attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    state.attributes[2].offset = offsetof(MaterialVertexLayout, tangent);

    state.attributes[3].location = 3;
    state.attributes[3].binding = 0;
    state.attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
    state.attributes[3].offset = offsetof(MaterialVertexLayout, uv);
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

// --- Per-field parameter classification [Task 7 coordinator addition 2] -
// The only shapes IRxMaterialInstance's setFloat/setFloat4/setTexture
// (rx_api.h) can ever bind: Float (setFloat), Float4 (setFloat4),
// TextureIndex (setTexture -- a plain `uint` field per this engine's own
// D8 convention: bindless texture indices are plain data inside TParams,
// never a resource-typed field). Ported verbatim from Task 6's own
// api_impl.cpp classifyFieldType() (that copy is now deleted -- see this
// file's own header comment) -- identical logic, now run once here against
// the authoritative already-linked program instead of a second session.
MaterialParamKind classifyFieldKind(slang::TypeLayoutReflection* typeLayout) {
    if (typeLayout == nullptr) {
        return MaterialParamKind::Unsupported;
    }
    switch (typeLayout->getKind()) {
        case slang::TypeReflection::Kind::Scalar: {
            switch (typeLayout->getScalarType()) {
                case slang::TypeReflection::ScalarType::Float32:
                    return MaterialParamKind::Float;
                case slang::TypeReflection::ScalarType::UInt32:
                    return MaterialParamKind::TextureIndex;
                default:
                    return MaterialParamKind::Unsupported;
            }
        }
        case slang::TypeReflection::Kind::Vector: {
            slang::TypeLayoutReflection* elementLayout = typeLayout->getElementTypeLayout();
            if (elementLayout == nullptr || typeLayout->getElementCount() != 4) {
                return MaterialParamKind::Unsupported;
            }
            if (elementLayout->getScalarType() == slang::TypeReflection::ScalarType::Float32) {
                return MaterialParamKind::Float4;
            }
            return MaterialParamKind::Unsupported;
        }
        default:
            return MaterialParamKind::Unsupported;
    }
}

// --- Bindless texture-sampling globals [Task 4, seed-10 carried] --------
// material.slang's own `gTextures`/`gSamplers` (unbounded arrays at set 0,
// the SAME external bindless set every other set-0 binding in this engine
// already uses -- see rx_rhi_vk::PipelineLayoutBuilder::build()'s own
// "EXTERNAL SET-0 SUBSTITUTION" comment) plus its `gMaterialGlobals` push
// constant (the real default-sampler bindless index -- see material.slang's
// own header comment). These are handled as OPTIONAL below (this walk does
// not hard-require any of the three, and would accept a hypothetical future
// material composite that omitted them) -- but verified directly against
// this project's shipped Slang build (not assumed) that in PRACTICE they are
// present in every material's own linked-program reflection today, whether
// or not that material's evaluate() ever calls rx_sampleTexture: `import
// material;` pulls in every one of material.slang's own top-level globals
// regardless of which of them the importing module's reachable entry-point
// code actually touches. See material.slang's own header comment, and
// test_material_system.cpp's reflection test (which asserts exactly this
// for test_unlit.slang, a material that never samples anything).
constexpr uint32_t kBindlessTextureArraySet = 0;

// Classifies a `DescriptorTableSlot`-category top-level global's element
// type -- mirrors rx_shader::reflect()'s own `mapElementType()` (same
// shipped Slang build, same verified masking technique for
// `SlangResourceShape`), narrowed to exactly the shapes material.slang
// declares (this file's own established discipline: reject anything this
// task's test list does not exercise rather than generalize speculatively
// -- see reflectMaterialLayout()'s own header comment on the identical
// choice for the ParameterBlock walk). [Phase 4 Task 16, D26.1] gained
// `StorageBufferArray` for material.slang's new `gDrawData` global
// (`StructuredBuffer<RxDrawData>[]`, unbounded, at
// BindlessTable::kStorageBufferBinding) -- SLANG_STRUCTURED_BUFFER is the
// same base-shape masking technique the two pre-existing cases already use,
// just a different `SlangResourceShape` value.
enum class BindlessArrayElementKind { SampledImageArray, SamplerArray, StorageBufferArray, Other };

BindlessArrayElementKind classifyBindlessArrayElement(slang::TypeReflection* elemType) {
    if (elemType == nullptr) {
        return BindlessArrayElementKind::Other;
    }
    if (elemType->getKind() == slang::TypeReflection::Kind::SamplerState) {
        return BindlessArrayElementKind::SamplerArray;
    }
    if (elemType->getKind() == slang::TypeReflection::Kind::Resource) {
        SlangResourceShape shape = elemType->getResourceShape();
        auto baseShape = static_cast<SlangResourceShape>(shape & 0x0F);
        bool combinedSampler = (shape & SLANG_TEXTURE_COMBINED_FLAG) != 0;
        if (baseShape == SLANG_TEXTURE_2D && !combinedSampler) {
            return BindlessArrayElementKind::SampledImageArray;
        }
        if (baseShape == SLANG_STRUCTURED_BUFFER) {
            return BindlessArrayElementKind::StorageBufferArray;
        }
    }
    return BindlessArrayElementKind::Other;
}

// Whether Slang reflects the global named `paramName` as a genuinely
// unbounded array (`SLANG_UNBOUNDED_SIZE`) -- the same
// `getBindingRangeBindingCount()` API rx_shader::reflect() already
// established as the only one that reports that sentinel correctly (see
// that file's own header comment), but correlated by NAME rather than by
// loop index: unlike reflect()'s own flat-globals-only walk, this
// program's top-level parameters also include a `ParameterBlock` (gParams,
// `SubElementRegisterSpace` category), and whether that consumes a slot in
// `getGlobalParamsTypeLayout()`'s own binding-range list (making an
// index-based correlation drift) is not a documented, verified fact this
// task tested -- a name search sidesteps the question entirely rather than
// assume either answer.
bool isReflectedAsUnboundedArray(slang::TypeLayoutReflection* globalTypeLayout, const char* paramName) {
    if (globalTypeLayout == nullptr || paramName == nullptr) {
        return false;
    }
    SlangInt bindingRangeCount = globalTypeLayout->getBindingRangeCount();
    for (SlangInt i = 0; i < bindingRangeCount; ++i) {
        slang::VariableReflection* leafVar = globalTypeLayout->getBindingRangeLeafVariable(i);
        const char* leafName = (leafVar != nullptr) ? leafVar->getName() : nullptr;
        if (leafName != nullptr && std::strcmp(leafName, paramName) == 0) {
            SlangInt bindingCount = globalTypeLayout->getBindingRangeBindingCount(i);
            return static_cast<size_t>(bindingCount) == SLANG_UNBOUNDED_SIZE;
        }
    }
    return false;
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
// -- Phase 3 materials support exactly one ParameterBlock<TParams> gParams
// at global scope, PLUS, as of Task 4, exactly the three optional bindless-
// sampling globals material.slang itself declares (`gTextures`/`gSamplers`
// at set 0, `gMaterialGlobals` as a push constant -- see that file's own
// header comment). Any OTHER kind of global (an ordinary flat resource this
// task did not declare, a second/differently-shaped push constant, ...) is
// a real, supportable future case (the underlying flat-resource reflection
// technique reflect() already uses is directly reusable for it), but
// nothing in this task's test list exercises one, and writing that mapping
// without a single verified case to check it against would be exactly the
// kind of unverified, speculative code this project's engineering
// discipline argues against -- so it is called out here, explicitly, as
// future work rather than shipped half-tested.
//
// [Task 7] Also walks gParams's element type's own FIELDS -- name, kind
// (classifyFieldKind() above), and byte offset/size within the block's own
// Uniform parameter category (`VariableLayoutReflection::getOffset()`/
// `TypeLayoutReflection::getSize()`, both defaulting to
// `slang::ParameterCategory::Uniform` -- exactly the category a
// ParameterBlock's own CPU-visible backing buffer uses) -- into
// `MaterialReflection::params`/`paramBlockSize`, the data
// MaterialSystem::materialParams()/paramBlockSize() expose and
// MaterialSystem::bindInstance() needs to lay out/size a real UBO.
struct MaterialReflection {
    rx::shader::ShaderLayoutInfo shaderLayout;
    std::vector<MaterialParamInfo> params;
    uint32_t paramBlockSize = 0;
};

std::optional<MaterialReflection> reflectMaterialLayout(slang::ProgramLayout* layout, const std::string& moduleLabel,
                                                         std::string& error) {
    MaterialReflection result;
    bool foundParamBlock = false;
    bool foundTextureArray = false;
    bool foundSamplerArray = false;
    bool foundDrawDataArray = false;
    bool foundComparisonSamplerArray = false;
    bool foundMaterialGlobalsPushConstant = false;

    // [Task 4] Needed only by the DescriptorTableSlot branch below, for
    // isReflectedAsUnboundedArray()'s name-correlated lookup -- computed
    // once here rather than per-parameter, matching rx_shader::reflect()'s
    // own precedent of hoisting this above its walk.
    slang::TypeLayoutReflection* globalTypeLayout = layout->getGlobalParamsTypeLayout();

    unsigned paramCount = layout->getParameterCount();
    for (unsigned i = 0; i < paramCount; ++i) {
        slang::VariableLayoutReflection* param = layout->getParameterByIndex(i);
        const char* rawName = param->getName();
        std::string name = (rawName != nullptr) ? rawName : "<anonymous>";
        slang::ParameterCategory category = param->getCategory();

        // --- Case 1: the material's own ParameterBlock<TParams> gParams,
        // at set 1 -- unchanged from Task 5/7 except for living inside this
        // new if/else-if chain instead of an early-reject.
        const bool isParamBlock = category == slang::ParameterCategory::SubElementRegisterSpace &&
                                   param->getTypeLayout() != nullptr &&
                                   param->getTypeLayout()->getKind() == slang::TypeReflection::Kind::ParameterBlock;
        if (isParamBlock) {
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
            result.shaderLayout.bindings.push_back(binding);
            foundParamBlock = true;

            // [Task 7] Field-level walk -- see this function's own header
            // comment for exactly what/why.
            slang::TypeLayoutReflection* blockTypeLayout = param->getTypeLayout();
            slang::TypeLayoutReflection* elementLayout =
                blockTypeLayout != nullptr ? blockTypeLayout->getElementTypeLayout() : nullptr;
            if (elementLayout != nullptr) {
                result.paramBlockSize =
                    static_cast<uint32_t>(elementLayout->getSize(slang::ParameterCategory::Uniform));
                unsigned fieldCount = elementLayout->getFieldCount();
                for (unsigned f = 0; f < fieldCount; ++f) {
                    slang::VariableLayoutReflection* field = elementLayout->getFieldByIndex(f);
                    if (field == nullptr) {
                        continue;
                    }
                    const char* fieldRawName = field->getName();
                    if (fieldRawName == nullptr) {
                        continue;
                    }
                    slang::TypeLayoutReflection* fieldTypeLayout = field->getTypeLayout();
                    MaterialParamInfo fieldInfo;
                    fieldInfo.name = fieldRawName;
                    fieldInfo.kind = classifyFieldKind(fieldTypeLayout);
                    fieldInfo.offset = static_cast<uint32_t>(field->getOffset(slang::ParameterCategory::Uniform));
                    fieldInfo.size =
                        fieldTypeLayout != nullptr
                            ? static_cast<uint32_t>(fieldTypeLayout->getSize(slang::ParameterCategory::Uniform))
                            : 0;
                    result.params.push_back(std::move(fieldInfo));
                }
            }
            continue;
        }

        // --- Case 2 [Task 4]: material.slang's own set-0 bindless arrays
        // (gTextures/gSamplers) -- handled as optional (this walk does not
        // require them), but in practice present for every material
        // regardless of whether its evaluate() actually calls
        // rx_sampleTexture -- see this function's own header comment.
        // Folded onto the SAME external BindlessTable every other set-0
        // binding in this engine already uses
        // (rx::rhi::PipelineLayoutBuilder::build()'s "EXTERNAL SET-0
        // SUBSTITUTION"), never a new descriptor set.
        if (category == slang::ParameterCategory::DescriptorTableSlot) {
            auto set = static_cast<uint32_t>(param->getBindingSpace());
            auto bindingIndex = static_cast<uint32_t>(param->getBindingIndex());
            slang::TypeReflection* elemType = param->getType() != nullptr ? param->getType()->unwrapArray() : nullptr;
            BindlessArrayElementKind kind = classifyBindlessArrayElement(elemType);
            bool unbounded = isReflectedAsUnboundedArray(globalTypeLayout, rawName);

            if (set == kBindlessTextureArraySet && bindingIndex == rx::rhi::BindlessTable::kSampledImageBinding &&
                kind == BindlessArrayElementKind::SampledImageArray && unbounded && !foundTextureArray) {
                rx::shader::ShaderLayoutInfo::Binding binding;
                binding.set = set;
                binding.binding = bindingIndex;
                binding.count = 0;
                binding.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                binding.stages = kMaterialStageFlags;
                binding.unboundedArray = true;
                result.shaderLayout.bindings.push_back(binding);
                foundTextureArray = true;
                continue;
            }
            if (set == kBindlessTextureArraySet && bindingIndex == rx::rhi::BindlessTable::kSamplerBinding &&
                kind == BindlessArrayElementKind::SamplerArray && unbounded && !foundSamplerArray) {
                rx::shader::ShaderLayoutInfo::Binding binding;
                binding.set = set;
                binding.binding = bindingIndex;
                binding.count = 0;
                binding.type = VK_DESCRIPTOR_TYPE_SAMPLER;
                binding.stages = kMaterialStageFlags;
                binding.unboundedArray = true;
                result.shaderLayout.bindings.push_back(binding);
                foundSamplerArray = true;
                continue;
            }
            // [Phase 4 Task 16, D26.1] material.slang's own `gDrawData`
            // bindless storage-buffer array -- SAME "handled as optional,
            // present for every material in practice" posture as
            // gTextures/gSamplers above (forward_entry.slang's vertexMain
            // reads it unconditionally, so every material composite pulls
            // it in regardless of whether the material's own evaluate()
            // ever touches it).
            if (set == kBindlessTextureArraySet && bindingIndex == rx::rhi::BindlessTable::kStorageBufferBinding &&
                kind == BindlessArrayElementKind::StorageBufferArray && unbounded && !foundDrawDataArray) {
                rx::shader::ShaderLayoutInfo::Binding binding;
                binding.set = set;
                binding.binding = bindingIndex;
                binding.count = 0;
                binding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                binding.stages = kMaterialStageFlags;
                binding.unboundedArray = true;
                result.shaderLayout.bindings.push_back(binding);
                foundDrawDataArray = true;
                continue;
            }
            // [Phase 4 Stage 2 Task 22 fix round, F1, spec D21] material.
            // slang's own `gShadowCompareSamplers` bindless comparison-
            // sampler array. Slang's reflection API does NOT distinguish
            // `SamplerComparisonState` from `SamplerState` by `TypeReflection::
            // Kind` -- both report `Kind::SamplerState` (verified directly
            // against this project's shipped slang.h: there is exactly one
            // `SLANG_TYPE_KIND_SAMPLER_STATE` value, no separate comparison-
            // sampler kind) -- so this case is keyed by BINDING NUMBER alone
            // (`kComparisonSamplerBinding`, 3), exactly like `gTextures`/
            // `gSamplers`/`gDrawData` above are each keyed by their own
            // fixed binding number. Vulkan itself draws no descriptor-TYPE
            // distinction either (VK_DESCRIPTOR_TYPE_SAMPLER either way) --
            // only the real `VkSampler`'s own `compareEnable` and this
            // shader-side type differ.
            if (set == kBindlessTextureArraySet && bindingIndex == rx::rhi::BindlessTable::kComparisonSamplerBinding &&
                kind == BindlessArrayElementKind::SamplerArray && unbounded && !foundComparisonSamplerArray) {
                rx::shader::ShaderLayoutInfo::Binding binding;
                binding.set = set;
                binding.binding = bindingIndex;
                binding.count = 0;
                binding.type = VK_DESCRIPTOR_TYPE_SAMPLER;
                binding.stages = kMaterialStageFlags;
                binding.unboundedArray = true;
                result.shaderLayout.bindings.push_back(binding);
                foundComparisonSamplerArray = true;
                continue;
            }

            error = "material module '" + moduleLabel + "' declares an unsupported bindless global '" + name +
                    "' at set " + std::to_string(set) + " binding " + std::to_string(bindingIndex) +
                    " (this engine recognizes only material.slang's own `gTextures` [unbounded Texture2D[], set " +
                    std::to_string(kBindlessTextureArraySet) + ", binding " +
                    std::to_string(rx::rhi::BindlessTable::kSampledImageBinding) +
                    "] / `gSamplers` [unbounded SamplerState[], set " + std::to_string(kBindlessTextureArraySet) +
                    ", binding " + std::to_string(rx::rhi::BindlessTable::kSamplerBinding) +
                    "] / `gDrawData` [unbounded StructuredBuffer<RxDrawData>[], set " +
                    std::to_string(kBindlessTextureArraySet) + ", binding " +
                    std::to_string(rx::rhi::BindlessTable::kStorageBufferBinding) +
                    "] / `gShadowCompareSamplers` [unbounded SamplerComparisonState[], set " +
                    std::to_string(kBindlessTextureArraySet) + ", binding " +
                    std::to_string(rx::rhi::BindlessTable::kComparisonSamplerBinding) + "] shape)";
            return std::nullopt;
        }

        // --- Case 3 [Task 4]: material.slang's own `gMaterialGlobals` push
        // constant (the real default-sampler bindless index -- see that
        // file's own header comment). Handled as optional, same reason as
        // Case 2 (and, empirically, present for every material for the
        // identical reason).
        if (category == slang::ParameterCategory::PushConstantBuffer) {
            if (foundMaterialGlobalsPushConstant) {
                error = "material module '" + moduleLabel +
                        "' declares more than one push-constant global ('" + name + "' is the second)";
                return std::nullopt;
            }

            slang::TypeLayoutReflection* wrapperLayout = param->getTypeLayout();
            slang::TypeLayoutReflection* elementLayout =
                wrapperLayout != nullptr ? wrapperLayout->getElementTypeLayout() : nullptr;
            const char* elementTypeName = elementLayout != nullptr ? elementLayout->getName() : nullptr;
            if (elementTypeName == nullptr || std::strcmp(elementTypeName, "RxMaterialGlobals") != 0) {
                error = "material module '" + moduleLabel + "' declares an unsupported push-constant global '" +
                        name + "' (this engine recognizes only material.slang's own `RxMaterialGlobals`)";
                return std::nullopt;
            }

            size_t elementSize =
                elementLayout != nullptr ? elementLayout->getSize(slang::ParameterCategory::Uniform) : 0;
            size_t offset = param->getOffset(slang::ParameterCategory::PushConstantBuffer);

            // [Phase 4 Task 16] bindInstance() (below) pushes exactly
            // `sizeof(rx::material::MaterialGlobalsPush)` bytes from a real
            // local of that type for this range -- a mismatch here would
            // make that call read past the end of that local. Reject loudly
            // (rather than let vkCmdPushConstants silently over/under-read)
            // if a future edit to material.slang's `RxMaterialGlobals`
            // struct shape ever changes its Slang-reflected size without
            // updating `rx::material::MaterialGlobalsPush` (draw_data.h) to
            // match byte-for-byte -- see that header's own comment on why
            // this drift risk is real and how both sides must move
            // together. All-scalar (uint/uint/float, 12 bytes) is expected
            // to reflect with zero padding, matching this project's own
            // empirically-verified "plain scalar push-constant fields pack
            // with zero padding" finding (shaders/multipass/lit.vert.slang's
            // own header comment) -- but this check verifies that against
            // the REAL shipped Slang build rather than assuming it, exactly
            // like the pre-Task-16 single-`uint` version of this check did.
            if (elementSize != sizeof(rx::material::MaterialGlobalsPush)) {
                error = "material module '" + moduleLabel +
                        "' links against a `RxMaterialGlobals` whose reflected size is " +
                        std::to_string(elementSize) + " bytes; this engine's bindInstance() only knows how to "
                        "push exactly " + std::to_string(sizeof(rx::material::MaterialGlobalsPush)) +
                        " bytes (rx::material::MaterialGlobalsPush) for it";
                return std::nullopt;
            }

            rx::shader::ShaderLayoutInfo::PushRange range;
            range.stages = kMaterialStageFlags;
            range.offset = static_cast<uint32_t>(offset);
            range.size = static_cast<uint32_t>(elementSize);
            result.shaderLayout.pushRanges.push_back(range);
            foundMaterialGlobalsPushConstant = true;
            continue;
        }

        error = "material module '" + moduleLabel + "' declares unsupported top-level global parameter '" + name +
                "' (Phase 3 materials support exactly one ParameterBlock<...> gParams and nothing else at global "
                "scope, plus, as of Task 4, material.slang's own optional bindless-sampling globals -- route "
                "textures through gParams's own bindless-table indices and rx_sampleTexture instead)";
        return std::nullopt;
    }

    if (!foundParamBlock) {
        error = "material module '" + moduleLabel +
                "' does not declare a ParameterBlock<...> gParams global (every material must bind its "
                "parameters through exactly one, at descriptor set " +
                std::to_string(kMaterialParamBlockSet) + ")";
        return std::nullopt;
    }
    return result;
}

// --- Pipeline variant cache key ------------------------------------------
// [D28, gate ruling RC1] `fixedFunctionState` joins the key alongside the
// three pre-D28 axes -- two materials sharing an identical module content
// hash, pass, and specialization bits but DIFFERENT AlphaMode/doubleSided
// values (i.e. two loadMaterial() calls against the same .slang bytes with
// two different MaterialFixedFunctionState arguments -- see that struct's
// own header comment) must yield two independently-cached VkPipelines.
struct PipelineKey {
    uint64_t moduleHash = 0;
    uint64_t passHash = 0;
    uint32_t specializationBits = 0;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false;

    bool operator==(const PipelineKey&) const = default;
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const noexcept {
        uint64_t h = kFnvOffsetBasis;
        fnv1aMix64(h, key.moduleHash);
        fnv1aMix64(h, key.passHash);
        fnv1aMix32(h, key.specializationBits);
        fnv1aMix32(h, static_cast<uint32_t>(key.alphaMode));
        fnv1aMix32(h, key.doubleSided ? 1U : 0U);
        return static_cast<size_t>(h);
    }
};

}  // namespace

// One loaded material: its retained linked Slang program (needed for
// getEntryPointCode() -- called once, right here, never again -- AND, as
// of Task 7, for future re-reflection: materialParams()/paramBlockSize()
// read straight from the SAME MaterialReflection computed at load/reload
// time, so linkedProgram itself is not re-queried after this record is
// built; it stays retained purely as a lifetime anchor and because a
// future task may need it), reflected layout, built pipeline layout, and
// the two VkShaderModules every getPipeline() call for this material
// reuses verbatim.
struct MaterialRecord {
    std::filesystem::path path;
    uint64_t contentHash = 0;

    // [Phase 4 Task 16, D28] The fixed-function pipeline-state axis this
    // material's loadMaterial() call was given (default: Opaque,
    // single-sided -- see MaterialFixedFunctionState's own header comment
    // for why that default is byte-identical to this class's pre-D28
    // hardcoded pipeline state). Read by getPipeline() below to build the
    // PipelineKey and the real VkPipeline blend/depth-write/cull state.
    MaterialFixedFunctionState fixedFunctionState;

    // [Task 7] Best-effort mtime baseline for reloadChanged()'s change
    // detection -- std::nullopt until the first successful stat (set at
    // load time below, and opportunistically by reloadChanged() itself if
    // load-time stat failed). A module reloadChanged() cannot currently
    // stat is skipped that call, not treated as "changed".
    std::optional<std::filesystem::file_time_type> lastKnownMtime;

    // [Task 7] Kept alive explicitly (belt-and-suspenders alongside
    // whatever internal reference `linkedProgram` itself may hold on its
    // owning session) so a reload's FRESH session -- necessarily a
    // DIFFERENT slang::ISession than the one `impl.session` still uses for
    // every OTHER material -- stays valid for exactly as long as this
    // record's own linkedProgram does. Every record's ownerSession is
    // independent; a reload never touches any OTHER material's session.
    Slang::ComPtr<slang::ISession> ownerSession;
    Slang::ComPtr<slang::IComponentType> linkedProgram;
    rx::shader::ShaderLayoutInfo layoutInfo;
    std::vector<MaterialParamInfo> params;  // [Task 7]
    uint32_t paramBlockSize = 0;            // [Task 7]
    rx::rhi::PipelineLayoutBundle layoutBundle;

    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;
};

// [Task 7 coordinator addition 4] One created texture: the real owned
// rx::rhi::Texture2D plus the BindlessTable slot its view was registered
// into. destroyAll()-equivalent teardown is handled entirely by RAII
// (Texture2D's own destructor) once ~MaterialSystem()'s vkDeviceWaitIdle()
// has already run, or by releaseTexture()'s own DeletionQueue-retired
// closure for a texture released mid-run.
struct TextureRecord {
    rx::rhi::Texture2D texture;
    rx::rhi::BindlessHandle bindlessHandle;
};

struct MaterialSystem::Impl {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    rx::rhi::BindlessTable* bindless = nullptr;
    std::filesystem::path pipelineCachePath;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;

    // [Task 8] The RESOLVED shared-shader directory this instance was
    // actually built with -- create()'s own `sharedShaderDir` parameter if
    // non-empty, else RX_MATERIAL_SHADER_DIR (see create()'s header comment
    // in material_system.h). Stashed here so reloadChanged()'s own fresh-
    // session path (which must recreate a session against the SAME shared
    // modules this instance loaded every OTHER material against) reads it
    // back rather than re-deriving it -- there is exactly one resolution
    // point (create()), matching every other "resolved once, reused" Impl
    // field in this struct.
    std::filesystem::path sharedShaderDir;

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
    // this instance will ever make -- do the same. reloadChanged() (Task 7)
    // follows the identical discipline: it walks this vector and re-fetches
    // each MaterialRecord* fresh via materials.get() every call, never
    // caching one across iterations.
    std::vector<MaterialHandle> materialHandles;

    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;

    // [Task 7 coordinator addition 4] Texture registry -- same
    // handle-not-pointer discipline as materialHandles above, for the
    // identical HandlePool-reallocation reason.
    rx::core::HandlePool<TextureTag, TextureRecord> textures;
    std::vector<TextureHandle> textureHandles;

    // [Task 7] GPU-bound instance state: this MaterialSystem's own internal
    // Allocator/Uploader (built once here, from `device`'s own
    // VkPhysicalDevice/VkDevice/VkInstance -- no separate Allocator&
    // parameter needed on create() itself) back both createTexture2D()'s
    // upload path and paramArena's per-frame host-visible buffers.
    // std::optional<...>, not a plain member: Allocator/Uploader/ParamArena
    // each have a private default constructor (their own create() factory
    // is the only way to build a real one -- see buffer.h/upload.h/
    // instance.h's own comments on why), so Impl (a different class) has
    // no accessible default to construct them with directly; std::optional
    // sidesteps that exactly like every GPU test fixture in this codebase
    // already does for the identical reason.
    std::optional<rx::rhi::Allocator> allocator;
    std::optional<rx::rhi::Uploader> uploader;
    std::optional<rx::material::ParamArena> paramArena;

    // [Task 4] This MaterialSystem's own single default LINEAR-filtering
    // sampler for rx_sampleTexture() -- see material.slang's own header
    // comment for why every material shares exactly one, and why its real
    // bindless index is carried via a push constant (written on every
    // bindInstance() call below) rather than a fixed/assumed slot number.
    // Created once in create(), destroyed in ~MaterialSystem() (after that
    // destructor's own vkDeviceWaitIdle(), same discipline as every other
    // GPU object it tears down).
    VkSampler defaultSampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle defaultSamplerHandle;

    // [Phase 4 Task 16, D26.1] This MaterialSystem's own single default
    // per-draw StructuredBuffer<RxDrawData> row (one identity-valued
    // DrawDataGpu -- model/normalMatrix/viewProj all identity,
    // light/ambient at neutral defaults) -- the SAME "process-wide default,
    // real bindless index carried via a push constant" idiom
    // `defaultSampler`/`defaultSamplerHandle` just above already
    // establishes, applied to material.slang's OTHER always-present set-0
    // global (`gDrawData`). Exists so bindInstance() below (the pre-D26.1
    // legacy path 06/07's own test materials still use) can push a real,
    // valid `drawDataBufferIndex` unconditionally -- forward_entry.slang's
    // vertexMain now ALWAYS reads `gDrawData[...][SV_VulkanInstanceID]`,
    // and every one of THOSE samples' draws has `firstInstance == 0`, so
    // this one identity row is exactly what keeps their pre-existing
    // CPU-pretransformed-position behavior byte-for-byte unchanged (see
    // forward_entry.slang's own header comment). Host-visible + persistently
    // mapped (Allocator::createHostVisibleBuffer()) -- written once, here,
    // via a direct memcpy + flush(), never through the Uploader (a single
    // 256-byte one-time setup write does not need staging-ring/ticket
    // machinery). Released/destroyed in ~MaterialSystem(), same
    // "vkDeviceWaitIdle() has already run, so an unconditional immediate
    // teardown is safe" discipline as defaultSampler/defaultSamplerHandle.
    std::optional<rx::rhi::Buffer> defaultDrawDataBuffer;
    rx::rhi::BindlessHandle defaultDrawDataBufferHandle;

    // Fence-gated deferred destruction for VkPipeline (reloadChanged()'s
    // own retirements) and rx::rhi::Texture2D (releaseTexture()'s own) --
    // [D9, bindless.h's release-safety contract]. `currentFrameNumber` is
    // the monotonic tag beginFrame() most recently stashed; every retire()
    // call in this file uses it.
    rx::rhi::DeletionQueue deletionQueue;
    uint64_t currentFrameNumber = 0;

    // Test-only seam -- see detail::debugCompileCount()'s own comment
    // (material_system.h). [Task 7] reloadChanged()'s own composite+link
    // attempts increment this too -- see that method's own comment.
    uint64_t compileCount = 0;
};

namespace detail {

uint64_t debugCompileCount(const MaterialSystem& system) { return system.impl_->compileCount; }

}  // namespace detail

namespace {

// Loads `filename` (one of the two fixed, shared files under `dir` --
// material.slang/forward_entry.slang; `dir` is MaterialSystem::create()'s
// own resolved effectiveShaderDir [Task 8] -- RX_MATERIAL_SHADER_DIR by
// default, or a caller-supplied runtime directory) as a module named after
// its own stem, exactly like rx_shader::Compiler's
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

// [Task 7] Factored out of what used to be inlined twice (create()'s
// own session setup, and reloadChanged()'s "fresh Compiler per reload"
// path) -- identical target/session configuration either way, which is
// exactly what task-6-report.md's Fix round 1 (F3) flagged as a real risk
// class when two independently-typed-out copies of this same setup drift:
// a single function makes drift structurally impossible rather than
// relying on two comments staying in sync by hand.
bool createMaterialSession(slang::IGlobalSession* globalSession, const std::filesystem::path& shaderDir,
                            Slang::ComPtr<slang::ISession>& outSession) {
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

    // [Task 8] `shaderDir` is the CALLER-resolved shared-shader directory
    // (MaterialSystem::create()'s own `sharedShaderDir` parameter, already
    // defaulted to RX_MATERIAL_SHADER_DIR by that method if the caller
    // passed none) -- this function itself no longer reads the macro
    // directly, so it cannot drift from create()'s own resolution.
    const std::string shaderDirStr = shaderDir.string();
    const char* searchPath = shaderDirStr.c_str();

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = &searchPath;
    sessionDesc.searchPathCount = 1;

    if (SLANG_FAILED(globalSession->createSession(sessionDesc, outSession.writeRef())) || outSession.get() == nullptr) {
        RX_LOG_ERROR("rx_material: IGlobalSession::createSession failed");
        return false;
    }
    return true;
}

// [Task 7] Also factored out for the identical create()/reloadChanged()
// reuse reason as createMaterialSession() above.
struct ForwardEntryBundle {
    slang::IModule* module = nullptr;  // owned by `session`, not this bundle.
    Slang::ComPtr<slang::IEntryPoint> vertexEntryPoint;
    Slang::ComPtr<slang::IEntryPoint> fragmentEntryPoint;
};

std::optional<ForwardEntryBundle> loadForwardEntry(slang::ISession* session, const std::filesystem::path& shaderDir,
                                                    std::string& diagnostics) {
    ForwardEntryBundle bundle;
    bundle.module = loadSharedModule(session, shaderDir, "forward_entry.slang", diagnostics);
    if (bundle.module == nullptr) {
        return std::nullopt;
    }
    if (SLANG_FAILED(bundle.module->findEntryPointByName("vertexMain", bundle.vertexEntryPoint.writeRef())) ||
        bundle.vertexEntryPoint.get() == nullptr) {
        RX_LOG_ERROR("rx_material: forward_entry.slang has no 'vertexMain' entry point");
        return std::nullopt;
    }
    if (SLANG_FAILED(bundle.module->findEntryPointByName("fragmentMain", bundle.fragmentEntryPoint.writeRef())) ||
        bundle.fragmentEntryPoint.get() == nullptr) {
        RX_LOG_ERROR("rx_material: forward_entry.slang has no 'fragmentMain' entry point");
        return std::nullopt;
    }
    return bundle;
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

// --- Shared compile core [Task 7] ---------------------------------------
// The composite/link/reflect/build/codegen/shader-module core BOTH
// loadMaterial() (against `impl.session`, reused across every material
// this MaterialSystem ever loads) and reloadChanged() (against a FRESH
// per-reload session -- see that method's own comment) now share, so the
// two paths cannot silently drift apart the way task-6-report.md's Fix
// round 1 (F3) found the old create()/reflectMaterialParams() duplication
// already had. Takes ownership of `session` (stored in the returned
// CompiledMaterial as its own lifetime anchor) -- callers on the
// persistent-session path pass a COPY of `impl.session` (Slang::ComPtr's
// own addRef-on-copy, cheap), never the original, so this never disturbs
// impl.session's own refcount/lifetime.
struct CompiledMaterial {
    Slang::ComPtr<slang::ISession> session;
    Slang::ComPtr<slang::IComponentType> linkedProgram;
    uint64_t contentHash = 0;
    rx::shader::ShaderLayoutInfo layoutInfo;
    std::vector<MaterialParamInfo> params;
    uint32_t paramBlockSize = 0;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;
};

void destroyCompiledMaterialGpuObjects(VkDevice device, CompiledMaterial& compiled) {
    if (compiled.vertexModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, compiled.vertexModule, nullptr);
        compiled.vertexModule = VK_NULL_HANDLE;
    }
    if (compiled.fragmentModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, compiled.fragmentModule, nullptr);
        compiled.fragmentModule = VK_NULL_HANDLE;
    }
    compiled.layoutBundle = rx::rhi::PipelineLayoutBundle{};
}

std::optional<CompiledMaterial> compileMaterial(VkDevice device, rx::rhi::BindlessTable& bindless,
                                                 Slang::ComPtr<slang::ISession> session,
                                                 slang::IModule* forwardEntryModule,
                                                 slang::IEntryPoint* vertexEntryPoint,
                                                 slang::IEntryPoint* fragmentEntryPoint,
                                                 const std::filesystem::path& slangModulePath,
                                                 std::string& diagnostics) {
    auto source = readFileBytes(slangModulePath);
    if (!source.has_value()) {
        diagnostics += "could not read material module '" + slangModulePath.string() + "'\n";
        return std::nullopt;
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
    // reloading the SAME path through the SAME session fails with Slang's
    // own "already loaded with different source" diagnostic -- exactly why
    // reloadChanged() below always calls this with a FRESH `session`
    // rather than `impl.session`.
    std::string moduleName = slangModulePath.stem().string();
    if (moduleName.empty()) {
        moduleName = "material";
    }

    Slang::ComPtr<slang::IBlob> sourceBlob(Slang::INIT_ATTACH, slang_createBlob(source->data(), source->size()));
    Slang::ComPtr<slang::IBlob> loadDiag;
    slang::IModule* materialModule = session->loadModuleFromSource(
        moduleName.c_str(), slangModulePath.string().c_str(), sourceBlob, loadDiag.writeRef());
    appendDiagnostics(diagnostics, "load", loadDiag);
    if (materialModule == nullptr) {
        return std::nullopt;
    }

    std::vector<slang::IComponentType*> parts;
    parts.push_back(forwardEntryModule);
    parts.push_back(vertexEntryPoint);
    parts.push_back(fragmentEntryPoint);
    parts.push_back(materialModule);

    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IBlob> composeDiag;
    SlangResult composeResult = session->createCompositeComponentType(
        parts.data(), static_cast<SlangInt>(parts.size()), composite.writeRef(), composeDiag.writeRef());
    appendDiagnostics(diagnostics, "compose", composeDiag);
    if (SLANG_FAILED(composeResult) || composite.get() == nullptr) {
        return std::nullopt;
    }

    Slang::ComPtr<slang::IComponentType> linked;
    Slang::ComPtr<slang::IBlob> linkDiag;
    SlangResult linkResult = composite->link(linked.writeRef(), linkDiag.writeRef());
    appendDiagnostics(diagnostics, "link", linkDiag);
    if (SLANG_FAILED(linkResult) || linked.get() == nullptr) {
        return std::nullopt;
    }

    Slang::ComPtr<slang::IBlob> layoutDiag;
    slang::ProgramLayout* progLayout = linked->getLayout(0, layoutDiag.writeRef());
    appendDiagnostics(diagnostics, "layout", layoutDiag);
    if (progLayout == nullptr) {
        return std::nullopt;
    }

    std::string reflectError;
    auto reflection = reflectMaterialLayout(progLayout, slangModulePath.string(), reflectError);
    if (!reflection.has_value()) {
        diagnostics += reflectError;
        diagnostics.push_back('\n');
        return std::nullopt;
    }

    auto layoutBundle =
        rx::rhi::PipelineLayoutBuilder::build(device, reflection->shaderLayout, bindless.descriptorSetLayout());
    if (!layoutBundle.has_value()) {
        diagnostics += "PipelineLayoutBuilder::build failed for material '" + slangModulePath.string() + "'\n";
        return std::nullopt;
    }

    CompiledMaterial result;
    result.session = std::move(session);
    result.linkedProgram = linked;
    result.contentHash = contentHash;
    result.layoutInfo = std::move(reflection->shaderLayout);
    result.params = std::move(reflection->params);
    result.paramBlockSize = reflection->paramBlockSize;
    result.layoutBundle = std::move(*layoutBundle);

    // Entry point order in the composite above is fixed
    // (forwardEntryModule, vertexEntryPoint, fragmentEntryPoint,
    // materialModule) -- vertex is always index 0, fragment always index 1,
    // matching compiler.cpp's own "entry points contribute in exactly the
    // order pushed" contract.
    Slang::ComPtr<slang::IBlob> vertexCodeDiag;
    Slang::ComPtr<slang::IBlob> vertexCode;
    SlangResult vertexCodeResult = linked->getEntryPointCode(0, 0, vertexCode.writeRef(), vertexCodeDiag.writeRef());
    appendDiagnostics(diagnostics, "codegen(vertex)", vertexCodeDiag);
    if (SLANG_FAILED(vertexCodeResult) || vertexCode.get() == nullptr) {
        destroyCompiledMaterialGpuObjects(device, result);
        return std::nullopt;
    }

    Slang::ComPtr<slang::IBlob> fragmentCodeDiag;
    Slang::ComPtr<slang::IBlob> fragmentCode;
    SlangResult fragmentCodeResult =
        linked->getEntryPointCode(1, 0, fragmentCode.writeRef(), fragmentCodeDiag.writeRef());
    appendDiagnostics(diagnostics, "codegen(fragment)", fragmentCodeDiag);
    if (SLANG_FAILED(fragmentCodeResult) || fragmentCode.get() == nullptr) {
        destroyCompiledMaterialGpuObjects(device, result);
        return std::nullopt;
    }

    VkShaderModuleCreateInfo vertexModuleInfo{};
    vertexModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexModuleInfo.codeSize = vertexCode->getBufferSize();
    vertexModuleInfo.pCode = static_cast<const uint32_t*>(vertexCode->getBufferPointer());
    if (vkCreateShaderModule(device, &vertexModuleInfo, nullptr, &result.vertexModule) != VK_SUCCESS) {
        diagnostics += "vkCreateShaderModule (vertex) failed for material '" + slangModulePath.string() + "'\n";
        destroyCompiledMaterialGpuObjects(device, result);
        return std::nullopt;
    }

    VkShaderModuleCreateInfo fragmentModuleInfo{};
    fragmentModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragmentModuleInfo.codeSize = fragmentCode->getBufferSize();
    fragmentModuleInfo.pCode = static_cast<const uint32_t*>(fragmentCode->getBufferPointer());
    if (vkCreateShaderModule(device, &fragmentModuleInfo, nullptr, &result.fragmentModule) != VK_SUCCESS) {
        diagnostics += "vkCreateShaderModule (fragment) failed for material '" + slangModulePath.string() + "'\n";
        destroyCompiledMaterialGpuObjects(device, result);
        return std::nullopt;
    }

    return result;
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

    // [Task 7] Runs every still-pending VkPipeline/Texture2D retirement
    // unconditionally now that the device is confirmed idle -- avoids
    // DeletionQueue's own destructor logging a spurious "never flushed"
    // error, and ensures a reload/texture-release that happened just
    // before shutdown does not leak its retired resource.
    impl.deletionQueue.flushAll();

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

    // [Stage 0 audit follow-on, found while regression-testing F2] A
    // still-live TextureRecord's rx::rhi::Texture2D must be torn down HERE,
    // explicitly, not left to plain RAII when `impl_` itself is destroyed
    // just after this function returns: `impl.textures` (this struct's
    // `HandlePool<TextureTag, TextureRecord>`) is declared BEFORE
    // `impl.allocator` above, so ordinary reverse-declaration-order member
    // destruction would tear `allocator` down FIRST -- while a still-live
    // Texture2D's own destructor (running afterward, as part of
    // `impl.textures`' own destruction) still needs that exact
    // `VmaAllocator` to call `vmaDestroyImage` through. That is a real,
    // reproducible use-after-free (confirmed via a scratch VMA debug build:
    // "Some allocations were not freed before destruction of this memory
    // block!" at `allocator`'s own teardown, followed by a crash once
    // `textures` finally runs its own destructors against the
    // already-destroyed allocator) for ANY texture a caller never
    // explicitly released before dropping the MaterialSystem -- e.g. an app
    // that shuts down with textures still bound to live materials, a
    // completely ordinary sequence nothing before this stopped to test.
    // See this destructor's own materialHandles loop just above for why
    // this re-derives each `TextureRecord*` fresh via `textures.get()`
    // rather than a vector of pointers gathered earlier -- identical
    // HandlePool-reallocation hazard, identical fix.
    //
    // Releasing the bindless slot unconditionally here (like
    // `defaultSamplerHandle` just below) rather than deferring it through
    // `deletionQueue` is correct specifically BECAUSE vkDeviceWaitIdle()
    // above already ran: bindless.h's own release-safety contract (see
    // releaseTexture()'s own comment for the general, still-in-flight
    // case F2 exists to fix) only requires deferring past the point no
    // in-flight command buffer could still be dynamically indexing the
    // slot -- device-idle already IS that point, unconditionally.
    for (TextureHandle handle : impl.textureHandles) {
        TextureRecord* record = impl.textures.get(handle);
        if (record == nullptr) {
            continue;  // already released (releaseTexture()) before shutdown.
        }
        if (record->bindlessHandle.isValid()) {
            impl.bindless->release(record->bindlessHandle);
        }
        // Moving the real Texture2D into this loop-scoped local and letting
        // it go out of scope at the end of THIS iteration runs its real
        // vkDestroyImageView/vmaDestroyImage teardown right here, while
        // `impl.allocator`/`impl.device` are unquestionably still alive --
        // never leaving it to `impl.textures`' own later, wrongly-ordered
        // implicit destruction.
        rx::rhi::Texture2D doomed = std::move(record->texture);
    }

    // [Task 4] Return the default sampler's slot to `bindless`'s free list
    // (safe -- see bindless.h's own RELEASE-SAFETY CONTRACT -- and good
    // hygiene, exactly like releaseTexture() already does for a texture's
    // own slot) before destroying the real VkSampler handle. Both are safe
    // unconditionally here specifically because vkDeviceWaitIdle() above
    // already confirmed no in-flight command buffer can still reference it.
    if (impl.defaultSamplerHandle.isValid()) {
        impl.bindless->release(impl.defaultSamplerHandle);
    }
    // [Phase 4 Task 16, D26.1] Same discipline, same safety justification
    // (vkDeviceWaitIdle() above), for the default draw-data buffer:
    // `reset()` explicitly destroys the contained rx::rhi::Buffer HERE
    // (vmaDestroyBuffer against its own stored VmaAllocator handle -- a
    // Buffer is self-contained, unlike Texture2D, so this does not depend
    // on `impl.allocator`'s own member-destruction order either way, but
    // resetting explicitly here keeps every GPU-object teardown in this
    // destructor visibly in one place rather than split between explicit
    // and implicit).
    if (impl.defaultDrawDataBufferHandle.isValid()) {
        impl.bindless->release(impl.defaultDrawDataBufferHandle);
    }
    impl.defaultDrawDataBuffer.reset();
    if (impl.defaultSampler != VK_NULL_HANDLE) {
        vkDestroySampler(impl.device, impl.defaultSampler, nullptr);
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
                                                         const std::filesystem::path& pipelineCachePath,
                                                         const std::filesystem::path& sharedShaderDir,
                                                         VkSamplerAddressMode defaultSamplerAddressMode) {
    rx::core::log::init();

    // [Task 8] Resolve ONCE, here: `sharedShaderDir` if the caller passed a
    // real one, else the original compile-time RX_MATERIAL_SHADER_DIR
    // default -- see this method's own header comment (material_system.h)
    // for the redistribution problem this solves. Every use of "the shared-
    // shader directory" below and in reloadChanged() (via
    // impl->sharedShaderDir) flows from this one resolution.
    const std::filesystem::path effectiveShaderDir =
        !sharedShaderDir.empty() ? sharedShaderDir : std::filesystem::path(RX_MATERIAL_SHADER_DIR);

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
    // effectiveShaderDir/material.slang without this class needing to
    // pre-load it explicitly. [Task 7] Factored into createMaterialSession()
    // so reloadChanged()'s fresh-session path cannot drift from this one.
    Slang::ComPtr<slang::ISession> session;
    if (!createMaterialSession(globalSession.get(), effectiveShaderDir, session)) {
        return nullptr;
    }

    std::string diagnostics;
    auto forwardEntry = loadForwardEntry(session, effectiveShaderDir, diagnostics);
    if (!forwardEntry.has_value()) {
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

    // [Task 7] This MaterialSystem's own Allocator/Uploader -- built
    // straight from `device`'s own already-stored handles (no new create()
    // parameter needed: Device already exposes physicalDevice()/device()/
    // instance(), exactly like rx::graph::Executor::create(Device&) already
    // does for its own internal Allocator, per that class's own comment on
    // Device::instance()).
    auto allocator =
        rx::rhi::Allocator::createRaw(device.physicalDevice(), device.device(), device.instance());
    if (!allocator.has_value()) {
        RX_LOG_ERROR("rx_material: rx::rhi::Allocator::createRaw failed");
        vkDestroyPipelineCache(device.device(), pipelineCache, nullptr);
        return nullptr;
    }
    auto uploader = rx::rhi::Uploader::create(*allocator, device);
    if (!uploader.has_value()) {
        RX_LOG_ERROR("rx_material: rx::rhi::Uploader::create failed");
        vkDestroyPipelineCache(device.device(), pipelineCache, nullptr);
        return nullptr;
    }
    auto paramArena = rx::material::ParamArena::create(device.device(), *allocator, MaterialSystem::kFramesInFlight);
    if (!paramArena.has_value()) {
        RX_LOG_ERROR("rx_material: rx::material::ParamArena::create failed");
        vkDestroyPipelineCache(device.device(), pipelineCache, nullptr);
        return nullptr;
    }

    // [Task 4, superseded -- Fix round sampler-wrap P0] The single default
    // LINEAR-filtering sampler `rx_sampleTexture()`'s TWO-ARGUMENT overload
    // samples with (material.slang's own header comment has the current,
    // correct account) -- created and registered exactly once here.
    // LINEAR + no LOD clamp matches this codebase's own already-established
    // "general-purpose color texture" sampler recipe (samples/
    // 03_bindless_mesh's own `linearInfo`) -- as opposed to sample 05's own
    // NEAREST + CLAMP_TO_EDGE shadow/tonemap sampler, which has its own
    // depth-comparison/1:1-copy reasons to differ.
    //
    // WRAP MODE -- `defaultSamplerAddressMode` (this method's own
    // parameter, default `VK_SAMPLER_ADDRESS_MODE_REPEAT`), NOT the
    // CLAMP_TO_EDGE this used to be hardcoded to: this WAS a real, shipped
    // defect (not a hypothetical) -- this was this engine's ONE
    // process-wide sampler EVERY material's EVERY texture slot sampled
    // through, unconditionally, regardless of that texture's own glTF wrap
    // mode. DamagedHelmet's own TEXCOORD_0 V lies wholly in
    // [1.0005, 1.9987] (glTF-spec-legal, relying on the glTF-default REPEAT
    // wrap) -- under the old hardcoded CLAMP_TO_EDGE, every fragment's V
    // clamped to 1.0 and the whole mesh sampled each texture's bottom edge
    // row (black dome, green jaw blob, zero emissive -- see
    // helmet-texture-fix-report.md's supersede section for the full
    // account). REPEAT is also glTF's OWN documented default ("sampler
    // unspecified" -> repeat wrapping, auto filtering) -- the spec-correct
    // choice for this fallback now that real per-slot sampler wiring
    // (StandardPbrParams/UnlitParams' own `*Sampler` fields,
    // TextureCache::getOrCreateSamplerBindlessIndex()) is what every
    // shipped material actually samples through; this process-wide default
    // is now ONLY the two-argument overload's fallback and
    // resolveSamplerIndex()'s own belt-and-suspenders fallback (samples/
    // 08_gltf_viewer/main.cpp), never the sole sampler for real content.
    //
    // Task 4's own seam-bleed GPU test (test_api_factory.cpp) still needs
    // CLAMP_TO_EDGE for its own deliberately non-tiled 2x2 test texture
    // (REPEAT's wraparound blends the wrong-edge neighbor into any sample
    // near a UV boundary -- empirically confirmed, task-4-report.md's own
    // "What went wrong once" section) -- it now passes
    // `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE` explicitly via this
    // parameter instead of inheriting it from this no-longer-CLAMP default.
    VkSamplerCreateInfo defaultSamplerInfo{};
    defaultSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    defaultSamplerInfo.magFilter = VK_FILTER_LINEAR;
    defaultSamplerInfo.minFilter = VK_FILTER_LINEAR;
    defaultSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    defaultSamplerInfo.addressModeU = defaultSamplerAddressMode;
    defaultSamplerInfo.addressModeV = defaultSamplerAddressMode;
    defaultSamplerInfo.addressModeW = defaultSamplerAddressMode;
    defaultSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler defaultSampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device.device(), &defaultSamplerInfo, nullptr, &defaultSampler) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_material: vkCreateSampler (default material sampler) failed");
        vkDestroyPipelineCache(device.device(), pipelineCache, nullptr);
        return nullptr;
    }
    rx::rhi::BindlessHandle defaultSamplerHandle = bindless.registerSampler(defaultSampler);
    if (!defaultSamplerHandle.isValid()) {
        RX_LOG_ERROR("rx_material: BindlessTable::registerSampler (default material sampler) failed");
        vkDestroySampler(device.device(), defaultSampler, nullptr);
        vkDestroyPipelineCache(device.device(), pipelineCache, nullptr);
        return nullptr;
    }

    // [Phase 4 Task 16, D26.1] The default per-draw StructuredBuffer row --
    // see Impl::defaultDrawDataBuffer's own comment for why this exists and
    // why every field below is left at its identity/neutral default.
    auto defaultDrawDataBuffer = allocator->createHostVisibleBuffer(
        sizeof(DrawDataGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rx::rhi::MemoryCategory::Internal);
    if (!defaultDrawDataBuffer.has_value()) {
        RX_LOG_ERROR("rx_material: Allocator::createHostVisibleBuffer (default draw-data buffer) failed");
        vkDestroySampler(device.device(), defaultSampler, nullptr);
        vkDestroyPipelineCache(device.device(), pipelineCache, nullptr);
        return nullptr;
    }
    {
        DrawDataGpu identityRow{};
        std::memcpy(defaultDrawDataBuffer->mappedData(), &identityRow, sizeof(identityRow));
        defaultDrawDataBuffer->flush();
    }
    rx::rhi::BindlessHandle defaultDrawDataBufferHandle =
        bindless.registerStorageBuffer(defaultDrawDataBuffer->handle(), sizeof(DrawDataGpu));
    if (!defaultDrawDataBufferHandle.isValid()) {
        RX_LOG_ERROR("rx_material: BindlessTable::registerStorageBuffer (default draw-data buffer) failed");
        vkDestroySampler(device.device(), defaultSampler, nullptr);
        vkDestroyPipelineCache(device.device(), pipelineCache, nullptr);
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->physicalDevice = device.physicalDevice();
    impl->device = device.device();
    impl->bindless = &bindless;
    impl->pipelineCachePath = pipelineCachePath;
    impl->pipelineCache = pipelineCache;
    impl->sharedShaderDir = effectiveShaderDir;
    impl->globalSession = std::move(globalSession);
    impl->session = std::move(session);
    impl->forwardEntryModule = forwardEntry->module;
    impl->vertexEntryPoint = std::move(forwardEntry->vertexEntryPoint);
    impl->fragmentEntryPoint = std::move(forwardEntry->fragmentEntryPoint);
    impl->allocator = std::move(allocator);
    impl->uploader = std::move(uploader);
    impl->paramArena = std::move(paramArena);
    impl->defaultSampler = defaultSampler;
    impl->defaultSamplerHandle = defaultSamplerHandle;
    impl->defaultDrawDataBuffer = std::move(defaultDrawDataBuffer);
    impl->defaultDrawDataBufferHandle = defaultDrawDataBufferHandle;

    return std::unique_ptr<MaterialSystem>(new MaterialSystem(std::move(impl)));
}

MaterialHandle MaterialSystem::loadMaterial(const std::filesystem::path& slangModulePath,
                                             MaterialFixedFunctionState fixedFunctionState) {
    RX_ASSERT_MAIN_THREAD("MaterialSystem::loadMaterial");
    RX_ZONE;
    Impl& impl = *impl_;

    // Copies impl.session (Slang::ComPtr addRef, cheap) rather than moving
    // it -- this MaterialSystem keeps using impl.session for every FUTURE
    // loadMaterial() call too. See compileMaterial()'s own header comment.
    ++impl.compileCount;
    std::string diagnostics;
    auto compiled = compileMaterial(impl.device, *impl.bindless, Slang::ComPtr<slang::ISession>(impl.session),
                                     impl.forwardEntryModule, impl.vertexEntryPoint.get(),
                                     impl.fragmentEntryPoint.get(), slangModulePath, diagnostics);
    if (!compiled.has_value()) {
        RX_LOG_ERROR("rx_material: failed to load material '{}':\n{}", slangModulePath.string(), diagnostics);
        throw std::runtime_error("rx_material: failed to load material '" + slangModulePath.string() + "':\n" +
                                  diagnostics);
    }
    if (!diagnostics.empty()) {
        RX_LOG_WARN("rx_material: diagnostics while loading material '{}':\n{}", slangModulePath.string(),
                     diagnostics);
    }

    MaterialRecord record;
    record.path = slangModulePath;
    record.contentHash = compiled->contentHash;
    record.fixedFunctionState = fixedFunctionState;
    record.ownerSession = std::move(compiled->session);
    record.linkedProgram = std::move(compiled->linkedProgram);
    record.layoutInfo = std::move(compiled->layoutInfo);
    record.params = std::move(compiled->params);
    record.paramBlockSize = compiled->paramBlockSize;
    record.layoutBundle = std::move(compiled->layoutBundle);
    record.vertexModule = compiled->vertexModule;
    record.fragmentModule = compiled->fragmentModule;

    std::error_code mtimeError;
    auto mtime = std::filesystem::last_write_time(slangModulePath, mtimeError);
    if (!mtimeError) {
        record.lastKnownMtime = mtime;
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

MaterialFixedFunctionState MaterialSystem::fixedFunctionState(MaterialHandle handle) const {
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::fixedFunctionState: invalid or stale MaterialHandle");
    }
    return record->fixedFunctionState;
}

VkPipelineLayout MaterialSystem::pipelineLayout(MaterialHandle handle) const {
    // [Phase 4 exit fix wave, I3] `impl_->materials` is the SAME unguarded
    // HandlePool loadMaterial()/reloadChanged() mutate (reloadChanged()
    // destroys the old VkPipelineLayout immediately) -- reading it from a
    // worker chunk is a sibling of the Task-24 GeometryPool::bind()
    // violation, silent until now because this class's read accessors
    // carried no guard. Matches docs/threading.md's whole-class
    // main-thread-only posture for MaterialSystem.
    RX_ASSERT_MAIN_THREAD("MaterialSystem::pipelineLayout");
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::pipelineLayout: invalid or stale MaterialHandle");
    }
    return record->layoutBundle.layout;
}

const rx::shader::ShaderLayoutInfo& MaterialSystem::layoutInfo(MaterialHandle handle) const {
    // [Phase 4 exit fix wave, I3] See pipelineLayout()'s comment above.
    RX_ASSERT_MAIN_THREAD("MaterialSystem::layoutInfo");
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::layoutInfo: invalid or stale MaterialHandle");
    }
    return record->layoutInfo;
}

const std::vector<MaterialParamInfo>& MaterialSystem::materialParams(MaterialHandle handle) const {
    // [Phase 4 exit fix wave, I3] See pipelineLayout()'s comment above.
    RX_ASSERT_MAIN_THREAD("MaterialSystem::materialParams");
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::materialParams: invalid or stale MaterialHandle");
    }
    return record->params;
}

uint32_t MaterialSystem::paramBlockSize(MaterialHandle handle) const {
    // [Phase 4 exit fix wave, I3] See pipelineLayout()'s comment above.
    RX_ASSERT_MAIN_THREAD("MaterialSystem::paramBlockSize");
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::paramBlockSize: invalid or stale MaterialHandle");
    }
    return record->paramBlockSize;
}

void MaterialSystem::beginFrame(uint32_t frameInFlightIndex, uint64_t frameNumber) {
    // [Phase 4 exit fix wave, F5] beginFrame() mutates impl_->paramArena
    // state and writes impl_->currentFrameNumber, both unguarded. A worker
    // chunk calling this is the same violation class as Task 24's
    // GeometryPool::bind() finding and Task 7's other MaterialSystem read
    // accessors, now closed: docs/threading.md's whole-class main-thread-only
    // posture for MaterialSystem.
    RX_ASSERT_MAIN_THREAD("MaterialSystem::beginFrame");
    Impl& impl = *impl_;
    impl.paramArena->beginFrame(frameInFlightIndex);
    impl.currentFrameNumber = frameNumber;
}

void MaterialSystem::onFrameCompleted(uint64_t completedFrameNumber) {
    // [Phase 4 exit fix wave, F5] See beginFrame()'s comment above.
    RX_ASSERT_MAIN_THREAD("MaterialSystem::onFrameCompleted");
    impl_->deletionQueue.onFrameFenceSignaled(completedFrameNumber);
}

void MaterialSystem::bindInstance(VkCommandBuffer cmd, const rx::graph::PassContext& passContext,
                                   const InstanceBinding& binding) {
    RX_ASSERT_MAIN_THREAD("MaterialSystem::bindInstance");
    Impl& impl = *impl_;

    MaterialRecord* record = impl.materials.get(binding.material);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::bindInstance: invalid or stale MaterialHandle");
    }
    if (binding.paramData == nullptr) {
        throw std::invalid_argument("rx_material: MaterialSystem::bindInstance: binding.paramData must be non-null");
    }
    if (binding.paramSize != record->paramBlockSize) {
        throw std::invalid_argument(
            "rx_material: MaterialSystem::bindInstance: binding.paramSize (" + std::to_string(binding.paramSize) +
            ") does not match this material's reflected paramBlockSize (" +
            std::to_string(record->paramBlockSize) + ")");
    }

    PipelineRequest req{binding.material, passContext.passSignature(), /*specializationBits=*/0};
    VkPipeline pipeline = getPipeline(req);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // [Task 4, Phase 4 Task 16] Guarded on the reflected layout actually
    // declaring this push range -- true for every material in practice (see
    // reflectMaterialLayout()'s own header comment), but this still checks
    // rather than assumes it: pushing unconditionally against a pipeline
    // layout that turned out to have NO matching VkPushConstantRange would
    // be a validation error (vkCmdPushConstants against a range absent from
    // the bound layout), and this material's own reflected
    // `record->layoutInfo` is the authoritative source for whether that
    // range exists on THIS material's pipeline layout specifically -- not
    // an assumption this function should hardcode. See material.slang's own
    // header comment for why `defaultSamplerIndex` (not a fixed slot) is
    // the real registered index of MaterialSystem's own default sampler,
    // and Impl::defaultDrawDataBuffer's own comment for why
    // `drawDataBufferIndex` here is always this instance's own DEFAULT
    // (identity) row -- bindInstance() is the pre-D26.1 LEGACY path (06/07's
    // own simple test materials); a real D26.1 caller (StandardPBR/Unlit,
    // samples/08_gltf_viewer) drives its OWN real per-draw buffer and pushes
    // this same push-constant range itself, never through this method.
    // `exposure` stays 0.0 (neutral, `2^0 == 1`) here for the identical
    // reason -- bindInstance() callers never asked for exposure control.
    if (!record->layoutInfo.pushRanges.empty()) {
        const rx::shader::ShaderLayoutInfo::PushRange& range = record->layoutInfo.pushRanges[0];
        MaterialGlobalsPush push;
        push.defaultSamplerIndex = impl.defaultSamplerHandle.index();
        push.drawDataBufferIndex = impl.defaultDrawDataBufferHandle.index();
        push.exposure = 0.0F;
        vkCmdPushConstants(cmd, record->layoutBundle.layout, range.stages, range.offset, range.size, &push);
    }

    if (record->layoutBundle.setLayouts.size() <= kMaterialParamBlockSet) {
        throw std::runtime_error(
            "rx_material: MaterialSystem::bindInstance: material '" + record->path.string() +
            "' has no set-1 parameter-block descriptor set layout");
    }
    VkDescriptorSetLayout setLayout = record->layoutBundle.setLayouts[kMaterialParamBlockSet];

    VkDescriptorSet set = impl.paramArena->writeAndAllocate(setLayout, binding.paramData, binding.paramSize);
    if (set == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "rx_material: MaterialSystem::bindInstance: parameter arena exhausted for the current frame");
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, record->layoutBundle.layout,
                             /*firstSet=*/kMaterialParamBlockSet, 1, &set, 0, nullptr);
}

void MaterialSystem::reloadChanged() {
    RX_ASSERT_MAIN_THREAD("MaterialSystem::reloadChanged");
    RX_ZONE;
    Impl& impl = *impl_;
    // [Task 8] The SAME resolved directory create() built this instance
    // with (impl.sharedShaderDir) -- never re-reads RX_MATERIAL_SHADER_DIR
    // directly, so a MaterialSystem built against a caller-supplied
    // sharedShaderDir keeps reloading against that same directory, not the
    // compile-time default.
    const std::filesystem::path& shaderDir = impl.sharedShaderDir;

    for (MaterialHandle handle : impl.materialHandles) {
        // Re-fetched fresh every iteration -- see Impl::materialHandles'
        // own comment for why a cached pointer across iterations would be
        // unsafe (a HandlePool reallocation from an unrelated
        // loadMaterial() call elsewhere could invalidate it; reloadChanged()
        // itself never calls loadMaterial(), but the discipline is kept
        // uniform with every other accessor in this file regardless).
        MaterialRecord* record = impl.materials.get(handle);
        if (record == nullptr) {
            continue;  // released/stale -- nothing to reload.
        }

        std::error_code statError;
        auto mtime = std::filesystem::last_write_time(record->path, statError);
        if (statError) {
            // File momentarily missing/unreadable (e.g. an editor
            // mid-save) -- try again next call, not fatal [same "02
            // pattern" as samples/02_hotreload's own pollAndReloadIfChanged()].
            continue;
        }
        if (!record->lastKnownMtime.has_value()) {
            // No baseline yet (load-time stat failed, or this is the
            // first reloadChanged() call) -- establish one now without
            // treating "never observed before" as "changed".
            record->lastKnownMtime = mtime;
            continue;
        }
        if (mtime == *record->lastKnownMtime) {
            continue;  // unchanged since the last call.
        }
        record->lastKnownMtime = mtime;

        // --- Fresh Compiler per reload [D9; compiler.h's same-module-name
        // caveat] -- MaterialSystem's own analogue is a fresh
        // slang::ISession (this class drives raw Slang, not
        // rx_shader::Compiler, but the identical caveat applies: reloading
        // the SAME path -- SAME module name, by construction -- through
        // impl.session a second time fails with Slang's own "already
        // loaded with different source" diagnostic). The process's real
        // fixed cost, `impl.globalSession`'s stdlib load, is reused
        // unchanged -- only a new, cheap ISession is created here, mirroring
        // compiler.h's own documented "a fresh Compiler::create() call only
        // ever pays for a new, cheap ISession, never a new IGlobalSession"
        // cost model.
        Slang::ComPtr<slang::ISession> freshSession;
        if (!createMaterialSession(impl.globalSession.get(), shaderDir, freshSession)) {
            RX_LOG_WARN("rx_material: reloadChanged: failed to create a fresh Slang session for '{}'; keeping "
                        "last-good",
                        record->path.string());
            continue;
        }

        std::string forwardEntryDiag;
        auto freshForwardEntry = loadForwardEntry(freshSession, shaderDir, forwardEntryDiag);
        if (!freshForwardEntry.has_value()) {
            RX_LOG_WARN("rx_material: reloadChanged: failed to reload forward_entry.slang while reloading '{}'; "
                        "keeping last-good:\n{}",
                        record->path.string(), forwardEntryDiag);
            continue;
        }

        ++impl.compileCount;
        std::string diagnostics;
        auto compiled = compileMaterial(impl.device, *impl.bindless, freshSession, freshForwardEntry->module,
                                         freshForwardEntry->vertexEntryPoint.get(),
                                         freshForwardEntry->fragmentEntryPoint.get(), record->path, diagnostics);
        if (!compiled.has_value()) {
            // Keep-last-good: `record` has not been touched at all past
            // `lastKnownMtime` above -- reload failure is not a caller
            // error [D9, brief].
            RX_LOG_WARN("rx_material: hot-reload of '{}' failed, keeping last-good pipeline(s):\n{}",
                        record->path.string(), diagnostics);
            continue;
        }
        if (!diagnostics.empty()) {
            RX_LOG_WARN("rx_material: diagnostics while hot-reloading '{}':\n{}", record->path.string(),
                        diagnostics);
        }

        uint64_t oldHash = record->contentHash;

        // --- Erase + retire every pipeline-cache entry derived from the
        // OLD module hash [D9: "a changed hash invalidates exactly the
        // (material, pass) cache entries derived from that module"].
        std::vector<PipelineKey> staleKeys;
        for (const auto& [key, pipeline] : impl.pipelines) {
            if (key.moduleHash == oldHash) {
                staleKeys.push_back(key);
            }
        }
        for (const PipelineKey& key : staleKeys) {
            VkPipeline stalePipeline = impl.pipelines.at(key);
            impl.pipelines.erase(key);
            impl.deletionQueue.retire(
                [device = impl.device, stalePipeline]() { vkDestroyPipeline(device, stalePipeline, nullptr); },
                impl.currentFrameNumber);
        }

        // --- Destroy the OLD VkShaderModules immediately -- unlike
        // VkPipeline, a shader module (and, per the identical Vulkan
        // spec note, a pipeline layout/descriptor set layout) needs to
        // remain valid only through vkCreate*Pipelines/command-buffer
        // RECORDING that uses it, never through GPU EXECUTION -- so there
        // is no in-flight-command-buffer hazard here to defer against,
        // unlike VkPipeline itself (which command execution genuinely
        // does reference for its duration). record->layoutBundle's own
        // old VkPipelineLayout/VkDescriptorSetLayouts are destroyed by the
        // move-assignment below (PipelineLayoutBundle::operator= destroys
        // whatever it's overwriting FIRST -- see that struct's own header
        // comment, and samples/02_hotreload's destroyReloadablePipeline()
        // for the identical established pattern).
        if (record->vertexModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(impl.device, record->vertexModule, nullptr);
        }
        if (record->fragmentModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(impl.device, record->fragmentModule, nullptr);
        }

        // --- Swap in the new compiled material, preserving path/
        // lastKnownMtime (already updated above).
        record->contentHash = compiled->contentHash;
        record->ownerSession = std::move(compiled->session);
        record->linkedProgram = std::move(compiled->linkedProgram);
        record->layoutInfo = std::move(compiled->layoutInfo);
        record->params = std::move(compiled->params);
        record->paramBlockSize = compiled->paramBlockSize;
        record->layoutBundle = std::move(compiled->layoutBundle);
        record->vertexModule = compiled->vertexModule;
        record->fragmentModule = compiled->fragmentModule;

        RX_LOG_INFO("rx_material: hot-reload of '{}' succeeded (module hash {:#x} -> {:#x})", record->path.string(),
                    oldHash, record->contentHash);
    }
}

VkPipeline MaterialSystem::getPipeline(const PipelineRequest& req) {
    RX_ASSERT_MAIN_THREAD("MaterialSystem::getPipeline");
    RX_ZONE;
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

    PipelineKey key{record->contentHash, req.pass.hash(), req.specializationBits,
                     record->fixedFunctionState.alphaMode, record->fixedFunctionState.doubleSided};
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

    // [D28] cullMode: doubleSided -> VK_CULL_MODE_NONE (glTF core spec,
    // quoted directly in the gate matrix: "back-face culling is disabled
    // and double sided lighting is enabled"); single-sided (the default)
    // keeps this class's original VK_CULL_MODE_BACK_BIT literal.
    const bool doubleSided = record->fixedFunctionState.doubleSided;
    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = req.pass.samples;

    // [D28] alphaMode drives TWO of the three fixed-function axes here:
    // BLEND disables depth-write (D22's own text: "BLEND: blending on,
    // depth-write off, cull off" -- the "cull off" third of that triple is
    // handled by `doubleSided` above per this ticket's own gate ruling,
    // which keeps doubleSided and alphaMode as independent, composable
    // axes rather than forcing every BLEND material double-sided) and
    // enables standard straight-alpha (source-alpha,
    // one-minus-source-alpha) blending on every color attachment; OPAQUE
    // and MASK are pipeline-state-IDENTICAL to each other (MASK's cutoff
    // is a shader-side discard, D28's own header comment) and to this
    // class's pre-D28 hardcoded behavior.
    const bool blend = record->fixedFunctionState.alphaMode == AlphaMode::Blend;
    const bool hasDepth = req.pass.depthFormat != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = hasDepth ? VK_TRUE : VK_FALSE;
    depthStencilState.depthWriteEnable = (hasDepth && !blend) ? VK_TRUE : VK_FALSE;
    // [D13, gate ruling RC3 -- "compare-op fork resolved as option (a)":
    // GREATER_OR_EQUAL, not LESS. From Phase 4 Stage 2 on, every
    // MaterialSystem-built pipeline is a MAIN-CAMERA pipeline (reversed-Z:
    // near=1, far=0, per D13) -- the scene path's depth-only SHADOW-caster
    // pass is a SEPARATE pipeline built OUTSIDE MaterialSystem entirely
    // (Task 22, the shadow bridge: see its own shadow-caster pipeline
    // creation, which deliberately keeps VK_COMPARE_OP_LESS + a clear
    // value of 1.0 -- shadow maps stay STANDARD-Z per D13's own text,
    // "the cascades work in the techniques phase revisits" -- do NOT
    // "fix" that call site's compare op to match this one; they are
    // intentionally different conventions, not a shared bug). This is the
    // single literal flip RC3's own text names: "getPipeline() serves
    // only reversed-Z main-camera pipelines from Stage 2 on, so its
    // compare-op flips as one literal in Task 22 -- no compare-op cache
    // axis is built in Phase 4." Every caller of getPipeline() must pair
    // this with a Reversed-convention depth attachment (D29's
    // AttachmentDesc::depthConvention, clear=0.0) -- a Standard-convention
    // depth attachment used with a MaterialSystem pipeline from this point
    // on fails every depth test trivially (nothing ever draws).
    depthStencilState.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(req.pass.colorCount);
    for (auto& blendAttachment : blendAttachments) {
        blendAttachment.blendEnable = blend ? VK_TRUE : VK_FALSE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
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

TextureHandle MaterialSystem::createTexture2D(const TextureCreateInfo& info) {
    Impl& impl = *impl_;

    if (info.width == 0 || info.height == 0) {
        throw std::invalid_argument(
            "rx_material: MaterialSystem::createTexture2D: width/height must both be > 0");
    }
    if (info.pixels == nullptr || info.pixelBytes == 0) {
        throw std::invalid_argument(
            "rx_material: MaterialSystem::createTexture2D: pixels/pixelBytes must be non-null/non-zero");
    }

    // Texture2D::create() ORs in TRANSFER_DST_BIT (and TRANSFER_SRC_BIT
    // when mip-chaining) itself -- callers pass only the "real" usage this
    // texture needs at sample time, per that function's own comment.
    auto texture = rx::rhi::Texture2D::create(impl.physicalDevice, impl.device, *impl.allocator,
                                               VkExtent2D{info.width, info.height}, info.format,
                                               VK_IMAGE_USAGE_SAMPLED_BIT,
                                               /*requestedMipLevels=*/info.generateMips ? 0 : 1);
    if (!texture.has_value()) {
        throw std::runtime_error("rx_material: MaterialSystem::createTexture2D: rx::rhi::Texture2D::create failed");
    }

    if (!impl.uploader->uploadToImage(*texture, info.pixels, static_cast<VkDeviceSize>(info.pixelBytes),
                                       info.generateMips)) {
        throw std::runtime_error("rx_material: MaterialSystem::createTexture2D: Uploader::uploadToImage failed");
    }
    // [Phase 4 Task 11, spec D25] Uploader::flush() no longer blocks on its
    // own -- createTexture2D() is a public, synchronous API contract
    // (`rx_material/rx_api.h`: the returned handle is immediately safe to
    // bind/sample), so this call site explicitly wait()s on the ticket to
    // preserve that contract byte-identically, exactly like
    // MeshBuffers::create()'s own documented choice.
    impl.uploader->wait(impl.uploader->flush());

    rx::rhi::BindlessHandle bindlessHandle =
        impl.bindless->registerSampledImage(texture->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!bindlessHandle.isValid()) {
        throw std::runtime_error(
            "rx_material: MaterialSystem::createTexture2D: BindlessTable::registerSampledImage failed (capacity "
            "exhausted)");
    }

    TextureRecord record{std::move(*texture), bindlessHandle};
    TextureHandle handle = impl.textures.acquire(std::move(record));
    impl.textureHandles.push_back(handle);
    return handle;
}

uint32_t MaterialSystem::textureBindlessIndex(TextureHandle handle) const {
    TextureRecord* record = impl_->textures.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::textureBindlessIndex: invalid or stale TextureHandle");
    }
    return record->bindlessHandle.index();
}

void MaterialSystem::releaseTexture(TextureHandle handle) {
    Impl& impl = *impl_;
    TextureRecord* record = impl.textures.get(handle);
    if (record == nullptr) {
        RX_LOG_WARN("rx_material: MaterialSystem::releaseTexture: invalid or already-released TextureHandle; "
                    "ignoring");
        return;
    }

    // [Stage 0 audit F2] bindless.h's own RELEASE-SAFETY CONTRACT (see that
    // header's comment above BindlessTable::release()) is explicit that
    // release()-then-immediately-register() into the SAME freed index is a
    // spec violation unless the slot's return to the free list waits for
    // the same fence-confirmed point the real resource teardown already
    // waits for -- the table's free list is LIFO, so a premature release()
    // here would hand this exact slot back out to the very next
    // createTexture2D() call deterministically, not rarely, while a still
    // in-flight frame's draws could still be dynamically indexing it.
    // Folding the release() into the SAME DeletionQueue-retired closure as
    // the Texture2D teardown below (rather than calling it eagerly, right
    // here) defers both to the identical fence-confirmed point; the
    // closure runs on the main thread from onFrameCompleted(), so
    // BindlessTable::release()'s RX_ASSERT_MAIN_THREAD guard stays
    // satisfied exactly like it did when this call ran here directly.
    rx::rhi::BindlessTable* bindless = impl.bindless;
    rx::rhi::BindlessHandle bindlessHandle = record->bindlessHandle;

    // DeletionQueue's own std::function-copy-constructibility requirement
    // (deletion_queue.h's own header comment): wrap the moved-out,
    // move-only Texture2D in a shared_ptr so the retired closure is itself
    // copy-constructible.
    auto texturePtr = std::make_shared<rx::rhi::Texture2D>(std::move(record->texture));
    impl.textures.release(handle);
    impl.deletionQueue.retire(
        [bindless, bindlessHandle, texturePtr]() {
            // Returns the slot to bindless's free list only now that this
            // frame's fence has actually signaled -- see this function's
            // own comment above for why that timing is load-bearing.
            bindless->release(bindlessHandle);
            // Dropping the last shared_ptr ref destroys the Texture2D.
        },
        impl.currentFrameNumber);
}

}  // namespace rx::material
