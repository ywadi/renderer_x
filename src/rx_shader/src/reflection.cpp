#include <rx_shader/reflection.h>

#include "detail/global_session_mutex.h"

#include <rx_core/log.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace rx::shader {

namespace {

// Duplicates compiler.cpp's own (anonymous-namespace-private) mapStage()
// rather than sharing it: only the mutex in detail/global_session_mutex.h
// needed cross-TU sharing for this task, and a ~15-line, spec-fixed
// (VkShaderStageFlagBits values never change) switch costs less to
// duplicate than to hoist into a third shared header for one caller each.
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
            RX_LOG_WARN("rx_shader::reflect: no VkShaderStageFlagBits mapping for SlangStage {}; using VK_SHADER_STAGE_ALL",
                        static_cast<int>(stage));
            return VK_SHADER_STAGE_ALL;
    }
}

// Maps one resource-kind global parameter's *element* type (already
// unwrapped past any array) to the VkDescriptorType it needs. Verified
// directly against this shipped Slang build (v2026.14.1) + `spirv-dis` on
// the actual emitted SPIR-V for: SamplerState, ConstantBuffer<T>, Texture2D,
// a `Texture2D[]` unbounded array, StructuredBuffer<T>/RWStructuredBuffer<T>,
// RWTexture2D<T>, and Sampler2D (HLSL's combined-texture-sampler shorthand).
// Other resource shapes below (1D/3D/Cube textures, subpass inputs, byte-
// address buffers, texel buffers) are mapped per slang.h's documented
// `SlangResourceShape`/`SlangResourceAccess` semantics but were not
// separately smoke-tested this task -- flagged here so a future task
// exercising one of them knows to verify it the same way before trusting
// it blindly.
VkDescriptorType mapElementType(slang::TypeReflection* elemType, const char* paramName) {
    using Kind = slang::TypeReflection::Kind;
    switch (elemType->getKind()) {
        case Kind::SamplerState:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case Kind::ConstantBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        // Kind::ShaderStorageBuffer / Kind::TextureBuffer are distinct
        // top-level TypeReflection::Kind values (not sub-shapes of
        // Kind::Resource) per slang.h -- not observed from any case this
        // task exercised (StructuredBuffer<T>/RWStructuredBuffer<T> both
        // surfaced as Kind::Resource with resourceShape ==
        // SLANG_STRUCTURED_BUFFER instead, handled below), but mapped here
        // defensively in case a future Slang version or a shader construct
        // this task didn't try routes through them instead.
        case Kind::ShaderStorageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case Kind::TextureBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case Kind::Resource: {
            SlangResourceShape shape = elemType->getResourceShape();
            // Base shape values (SLANG_TEXTURE_1D=0x01 .. SLANG_TEXTURE_SUBPASS=0x0A)
            // are all < 0x10; every modifier (array/multisample/shadow/
            // feedback/combined-sampler) is OR'd in starting at 0x10, so
            // masking with 0x0F isolates the base shape cleanly [verified:
            // slang.h's SlangResourceShape enum values].
            auto baseShape = static_cast<SlangResourceShape>(shape & 0x0F);
            bool combinedSampler = (shape & SLANG_TEXTURE_COMBINED_FLAG) != 0;
            SlangResourceAccess access = elemType->getResourceAccess();
            bool readWrite = access != SLANG_RESOURCE_ACCESS_READ && access != SLANG_RESOURCE_ACCESS_NONE;
            switch (baseShape) {
                case SLANG_TEXTURE_1D:
                case SLANG_TEXTURE_2D:
                case SLANG_TEXTURE_3D:
                case SLANG_TEXTURE_CUBE:
                case SLANG_TEXTURE_SUBPASS:
                    if (combinedSampler) {
                        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    }
                    return readWrite ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                case SLANG_STRUCTURED_BUFFER:
                case SLANG_BYTE_ADDRESS_BUFFER:
                    // Vulkan has one descriptor type for both read-only and
                    // read-write raw/structured buffers (SSBOs) -- the
                    // read/write distinction lives in the SPIR-V access
                    // qualifiers on the binding, not the descriptor type.
                    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                case SLANG_TEXTURE_BUFFER:
                    return readWrite ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                default:
                    RX_LOG_WARN("rx_shader::reflect: unsupported resource shape 0x{:x} for global '{}'; skipping",
                                static_cast<unsigned>(shape), paramName != nullptr ? paramName : "?");
                    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
            }
        }
        default:
            RX_LOG_WARN("rx_shader::reflect: unsupported global-parameter type kind {} for '{}'; skipping",
                        static_cast<int>(elemType->getKind()), paramName != nullptr ? paramName : "?");
            return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

}  // namespace

std::optional<ShaderLayoutInfo> reflect(const CompileResult& result) {
    if (!result.ok || result.linkedProgram.get() == nullptr) {
        RX_LOG_ERROR("rx_shader::reflect: called on a CompileResult with ok=false or no linked program");
        return std::nullopt;
    }

    // Same lock Compiler::compileImpl takes for every front-end operation --
    // see detail/global_session_mutex.h and reflection.h's comment on
    // reflect() for why getLayout()/the walk below need it too.
    std::lock_guard<std::mutex> lock(detail::globalSessionMutex());

    Slang::ComPtr<slang::IBlob> layoutDiagnostics;
    slang::ProgramLayout* layout = result.linkedProgram->getLayout(0, layoutDiagnostics.writeRef());
    if (layoutDiagnostics.get() != nullptr && layoutDiagnostics->getBufferSize() > 0) {
        RX_LOG_WARN("rx_shader::reflect: diagnostics from getLayout:\n{}",
                    std::string(static_cast<const char*>(layoutDiagnostics->getBufferPointer()),
                                layoutDiagnostics->getBufferSize()));
    }
    if (layout == nullptr) {
        RX_LOG_ERROR("rx_shader::reflect: IComponentType::getLayout returned null");
        return std::nullopt;
    }

    // Stage-flag merge: every entry point's stage, OR'd together, applied to
    // every binding/push range. Slang's global-parameter reflection (used
    // below) does not surface which specific entry points reference a given
    // global, only where it's bound -- and over-approximating stage
    // visibility on a VkDescriptorSetLayoutBinding/VkPushConstantRange is
    // always spec-legal (Vulkan never rejects a stage flag broader than a
    // module's actual usage), so this is a safe, if conservative, default
    // for the flat, single-set-of-globals shape reflect() targets.
    VkShaderStageFlags allStages = 0;
    SlangUInt entryPointCount = layout->getEntryPointCount();
    for (SlangUInt i = 0; i < entryPointCount; ++i) {
        allStages |= static_cast<VkShaderStageFlags>(mapStage(layout->getEntryPointByIndex(i)->getStage()));
    }

    slang::TypeLayoutReflection* globalTypeLayout = layout->getGlobalParamsTypeLayout();
    SlangInt bindingRangeCount = (globalTypeLayout != nullptr) ? globalTypeLayout->getBindingRangeCount() : 0;

    ShaderLayoutInfo info;
    unsigned paramCount = layout->getParameterCount();
    for (unsigned i = 0; i < paramCount; ++i) {
        slang::VariableLayoutReflection* param = layout->getParameterByIndex(i);
        const char* paramName = param->getName();
        slang::ParameterCategory category = param->getCategory();

        if (category == slang::ParameterCategory::PushConstantBuffer) {
            // The wrapper type layout's own size (in the Uniform category)
            // is 0 regardless of spelling (`[[vk::push_constant]] ConstantBuffer<T> pc;`
            // or the bare-struct `[[vk::push_constant]] T pc;` form) -- verified
            // directly: both produce a ConstantBuffer-kind wrapper
            // TypeLayoutReflection with a non-null getElementTypeLayout()
            // holding the real struct size. Falling back to the wrapper's
            // own getSize() if that's ever null is defensive, not expected
            // to trigger against this shipped build.
            slang::TypeLayoutReflection* wrapperLayout = param->getTypeLayout();
            slang::TypeLayoutReflection* elementLayout =
                (wrapperLayout != nullptr) ? wrapperLayout->getElementTypeLayout() : nullptr;
            size_t size = (elementLayout != nullptr) ? elementLayout->getSize()
                                                      : ((wrapperLayout != nullptr) ? wrapperLayout->getSize() : 0);
            if (size == 0 || size == SLANG_UNBOUNDED_SIZE || size == SLANG_UNKNOWN_SIZE) {
                RX_LOG_WARN("rx_shader::reflect: push constant '{}' has an unresolvable size ({}); skipping",
                            paramName != nullptr ? paramName : "?", size);
                continue;
            }
            size_t offset = param->getOffset(category);
            if (offset == SLANG_UNKNOWN_SIZE) {
                RX_LOG_WARN("rx_shader::reflect: push constant '{}' has an unresolvable offset; skipping",
                            paramName != nullptr ? paramName : "?");
                continue;
            }

            ShaderLayoutInfo::PushRange range;
            range.stages = allStages;
            range.offset = static_cast<uint32_t>(offset);
            range.size = static_cast<uint32_t>(size);
            info.pushRanges.push_back(range);
            continue;
        }

        if (category != slang::ParameterCategory::DescriptorTableSlot) {
            // A plain (non-push-constant) global uniform, a ParameterBlock,
            // or any other category outside this walk's flat-globals scope
            // (see reflection.h's comment on reflect()) -- logged, never
            // silently dropped or mismapped.
            RX_LOG_WARN("rx_shader::reflect: global parameter '{}' has unsupported category {}; skipping",
                        paramName != nullptr ? paramName : "?", static_cast<int>(category));
            continue;
        }

        slang::TypeReflection* elemType = param->getType()->unwrapArray();
        VkDescriptorType vkType = mapElementType(elemType, paramName);
        if (vkType == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            continue;  // mapElementType already logged why.
        }

        // Count/unbounded-ness: deliberately NOT from
        // `TypeReflection::getElementCount()` -- see reflection.h's comment
        // on reflect() for the discrepancy this was found to have against
        // [R:A3]'s assumption. `getBindingRangeBindingCount()` on the
        // global-scope type layout is the API observed to report Slang's
        // `SLANG_UNBOUNDED_SIZE` sentinel correctly; it's correlated to this
        // parameter by index *and* leaf-variable name (both matched in
        // every case this task tested, for two independently-shaped test
        // shaders) so a future mismatch fails loudly instead of silently
        // mixing up two bindings' counts.
        if (static_cast<SlangInt>(i) >= bindingRangeCount) {
            RX_LOG_ERROR("rx_shader::reflect: no binding-range entry for global parameter '{}' (index {}); skipping",
                         paramName != nullptr ? paramName : "?", i);
            continue;
        }
        slang::VariableReflection* leafVar = globalTypeLayout->getBindingRangeLeafVariable(static_cast<SlangInt>(i));
        const char* leafName = (leafVar != nullptr) ? leafVar->getName() : nullptr;
        if (leafName == nullptr || paramName == nullptr || std::strcmp(leafName, paramName) != 0) {
            RX_LOG_ERROR(
                "rx_shader::reflect: binding-range/parameter name mismatch at index {} ('{}' vs '{}'); skipping",
                i, paramName != nullptr ? paramName : "?", leafName != nullptr ? leafName : "?");
            continue;
        }

        SlangInt bindingCount = globalTypeLayout->getBindingRangeBindingCount(static_cast<SlangInt>(i));
        bool unbounded = static_cast<size_t>(bindingCount) == SLANG_UNBOUNDED_SIZE;

        ShaderLayoutInfo::Binding binding;
        binding.set = static_cast<uint32_t>(param->getBindingSpace());
        binding.binding = static_cast<uint32_t>(param->getBindingIndex());
        binding.count = unbounded ? 0 : static_cast<uint32_t>(bindingCount);
        binding.type = vkType;
        binding.stages = allStages;
        binding.unboundedArray = unbounded;
        info.bindings.push_back(binding);
    }

    return info;
}

}  // namespace rx::shader
