#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace rx::shader {

// Plain-data output of reflect() (reflection.h): every set/binding/type/
// count/stage and push-constant range a compiled Slang program needs,
// expressed purely in Vulkan + standard-library types. This is deliberately
// the ONLY rx_shader header rx_rhi_vk's PipelineLayoutBuilder (rx_rhi_vk/
// pipeline_layout.h) includes -- no Slang type (slang::*, Slang::ComPtr<...>,
// SlangResult, ...) appears anywhere in this file, on purpose: rx_rhi_vk
// must never link slang::slang or expose a Slang type from its own headers
// [Task 2 brief, spec Components table -- rx_rhi_vk's listed dependencies
// are "Phase 1 RHI, VMA", not rx_shader/slang]. See
// rx_shader/CMakeLists.txt's `rx_shader_layout_types` INTERFACE target: it
// exposes exactly this header's include directory (nothing else -- not
// slang::slang, not compiler.h/reflection.h) to rx_rhi_vk, which is what
// makes that separation a build-graph guarantee rather than a convention
// someone could accidentally violate with one stray #include.
//
// This struct carries NO matrix-layout information -- for the default
// row-major-vs-column-major matrix packing this Compiler's sessions use
// (a real, easy-to-miss gotcha for a `float4x4` read out of a buffer), see
// compiler.h's doc comment on `Compiler::create()`.
struct ShaderLayoutInfo {
    // One VkDescriptorSetLayoutBinding's worth of information, before it's
    // actually turned into one by PipelineLayoutBuilder.
    struct Binding {
        uint32_t set = 0;
        uint32_t binding = 0;

        // Descriptor array length. Meaningless (left at 0) when
        // `unboundedArray` is true: Slang's own reflection reports
        // `SLANG_UNBOUNDED_SIZE` (`~size_t(0)`) for a genuinely unsized
        // `T x[]` global, which has no representation in a uint32_t.
        // PipelineLayoutBuilder picks its own upper bound for
        // VkDescriptorSetLayoutBinding::descriptorCount in that case -- see
        // its header for why and what.
        uint32_t count = 0;

        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;

        // Every shader stage in the linked program that could reach this
        // binding. reflect() merges stage flags conservatively (every
        // entry point's stage, not just the ones provably touching this
        // exact global) -- see reflection.h's comment on reflect() for why
        // that is always safe for VkDescriptorSetLayoutBinding::stageFlags
        // even when it over-approximates.
        VkShaderStageFlags stages = 0;

        // True for a runtime-sized array (`Texture2D g[]`, no bound) --
        // PipelineLayoutBuilder gives these
        // UPDATE_AFTER_BIND_BIT | PARTIALLY_BOUND_BIT binding flags plus the
        // set-level UPDATE_AFTER_BIND_POOL_BIT layout flag, so they can be
        // written into after binding (Task 3's bindless table depends on
        // this).
        bool unboundedArray = false;
    };
    std::vector<Binding> bindings;

    // One VkPushConstantRange's worth of information.
    struct PushRange {
        VkShaderStageFlags stages = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };
    std::vector<PushRange> pushRanges;
};

}  // namespace rx::shader
