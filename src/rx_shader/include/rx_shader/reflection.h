#pragma once
#include <rx_shader/compiler.h>
#include <rx_shader/shader_layout_info.h>
#include <optional>

namespace rx::shader {

// Walks a successfully-linked CompileResult's Slang reflection data
// (`slang::ProgramLayout`, obtained from `CompileResult::linkedProgram`)
// and produces the plain-struct `ShaderLayoutInfo`
// `rx::rhi::PipelineLayoutBuilder` (rx_rhi_vk) consumes to build a matching
// `VkDescriptorSetLayout`/`VkPipelineLayout`. Returns `std::nullopt` (logging
// why via RX_LOG_ERROR) if `result.ok` is false, if `result.linkedProgram`
// is null, or if `getLayout()` itself fails.
//
// Scope: flat global-scope resource declarations (individual textures,
// samplers, buffers, and runtime-sized arrays thereof, each with an
// explicit `[[vk::binding(binding, set)]]`) plus at most a few
// `[[vk::push_constant]]`-attributed globals. Slang's `ParameterBlock<T>`/
// nested-parameter-block layouts are out of scope for this walk (Task 2's
// interfaces and tests never exercise them; the spec's Fixed decision #4
// notes "no intermediate metadata format exists in this design" -- this is
// that design, at exactly the granularity the samples need) -- a global
// parameter reflect() cannot classify is skipped with an RX_LOG_WARN/ERROR,
// never silently mismapped.
//
// Ground truth vs [R:A3]: the research file describes deriving descriptor
// sets/ranges by walking `TypeLayoutReflection::getDescriptorSetCount()` /
// `getDescriptorSetDescriptorRange*()` on `getGlobalParamsTypeLayout()`.
// Direct, empirical testing against the shipped v2026.14.1 `slang.h` (a
// throwaway probe compiling a shader with two explicit descriptor sets,
// cross-checked against `spirv-dis` on the real emitted SPIR-V) found that
// API's *type* and *count* fields correct, but its per-range *descriptor
// set* attribution wrong for at least one real case (a `ConstantBuffer<T>`
// explicitly bound to `[[vk::binding(1, 1)]]` was reported under descriptor
// set index 0's range list, not set 1's, even though the real SPIR-V
// unambiguously decorates it `DescriptorSet 1` and the top-level parameter
// walk below reports `getBindingSpace() == 1` correctly for the same
// variable). Per this task's brief ("the shipped slang.h wins over the
// research file on any disagreement; note the discrepancy in the report"),
// reflect() therefore does NOT use that per-range set/binding attribution
// at all. Instead it combines two verified-correct sources:
//   - `ProgramLayout::getParameterByIndex()` (flat, top-level global scope)
//     for set (`getBindingSpace()`), binding (`getBindingIndex()`),
//     category, and name -- verified byte-for-byte against `spirv-dis`
//     output for every case reflection_test.cpp exercises.
//   - `TypeLayoutReflection::getBindingRangeType()` /
//     `getBindingRangeBindingCount()` on `getGlobalParamsTypeLayout()`,
//     correlated to the same global parameter by index *and* leaf-variable
//     name (a defensive cross-check, not just trust-the-index), for
//     descriptor type and array count/unbounded-ness -- this is the only
//     API observed to correctly report Slang's `SLANG_UNBOUNDED_SIZE`
//     sentinel for a genuinely unsized `T x[]` global;
//     `TypeReflection::getElementCount()` (with or without a reflection
//     context) was observed returning `0` or `2147483647` for the exact
//     same unbounded array, neither of which is that sentinel.
// See reflection.cpp for the full walk and the resource-kind -> VkDescriptorType
// mapping table (verified for Sampler/Texture2D/CombinedSampler/
// ConstantBuffer/StructuredBuffer/RWStructuredBuffer/RWTexture2D against
// this same shipped build; other resource shapes are mapped per slang.h's
// documented enum semantics but were not separately smoke-tested this
// task).
//
// Threading: takes the same process-wide mutex `Compiler` locks for every
// front-end operation (module load/compose/link) -- see
// src/detail/global_session_mutex.h's comment for why a reflection walk
// needs that same external synchronization, not a separate or absent lock.
std::optional<ShaderLayoutInfo> reflect(const CompileResult& result);

}  // namespace rx::shader
