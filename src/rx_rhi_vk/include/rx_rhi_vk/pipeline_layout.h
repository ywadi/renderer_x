#pragma once
#include <volk.h>
// The ONLY rx_shader header this file (or any rx_rhi_vk header) includes --
// a plain-data struct with zero Slang types and zero slang::slang link
// dependency. See that header's own comment, and
// rx_shader/CMakeLists.txt's `rx_shader_layout_types` INTERFACE target,
// for how that separation is enforced at the build-graph level: this
// library links `rx_shader_layout_types`, never `rx_shader` itself.
#include <rx_shader/shader_layout_info.h>
#include <optional>
#include <vector>

namespace rx::rhi {

class PipelineLayoutBuilder;

// RAII owner of every Vulkan object PipelineLayoutBuilder::build() creates:
// one VkDescriptorSetLayout per descriptor set index the shader's
// ShaderLayoutInfo uses (including empty placeholder layouts for any gap --
// see build()'s comment), and the VkPipelineLayout built from them plus the
// shader's push-constant ranges. Move-only; destroys everything via
// vkDestroyPipelineLayout/vkDestroyDescriptorSetLayout on destruction or
// move-assignment.
//
// `setLayouts`/`layout` are public (not private-with-accessor, unlike most
// RAII types in this library) because callers need them directly: `layout`
// for vkCreateGraphicsPipelines/vkCmdBindDescriptorSets'
// firstSet/pDescriptorSets bookkeeping, `setLayouts` for allocating actual
// VkDescriptorSets against each one. Callers must not destroy either
// themselves -- this bundle owns both.
struct PipelineLayoutBundle {
    std::vector<VkDescriptorSetLayout> setLayouts;
    VkPipelineLayout layout = VK_NULL_HANDLE;

    PipelineLayoutBundle() = default;
    PipelineLayoutBundle(PipelineLayoutBundle&&) noexcept;
    PipelineLayoutBundle& operator=(PipelineLayoutBundle&&) noexcept;
    PipelineLayoutBundle(const PipelineLayoutBundle&) = delete;
    PipelineLayoutBundle& operator=(const PipelineLayoutBundle&) = delete;
    ~PipelineLayoutBundle();

private:
    friend class PipelineLayoutBuilder;

    explicit PipelineLayoutBundle(VkDevice device) : device_(device) {}

    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;
};

// Turns a shader's reflected ShaderLayoutInfo (rx_shader's reflect() output
// -- see that header) into a matching VkDescriptorSetLayout per descriptor
// set index plus the VkPipelineLayout tying them together with the
// shader's push-constant ranges. Stateless: every method is static, there
// is nothing to construct.
class PipelineLayoutBuilder {
public:
    PipelineLayoutBuilder() = delete;

    // Upper bound used for VkDescriptorSetLayoutBinding::descriptorCount
    // when a binding's `unboundedArray` is true -- Vulkan requires a
    // concrete (nonzero) descriptorCount at layout-creation time even for
    // an UPDATE_AFTER_BIND | PARTIALLY_BOUND binding (those flags relax
    // *which* descriptors within that count must be valid/written, not
    // whether a count is needed at all). This is a generic default sized
    // for this builder's own reflection-driven path in isolation; Task 3's
    // BindlessTable defines its own explicit per-resource-class capacities
    // (1024/16/256 sampled images/samplers/storage buffers, per its own
    // brief) and is expected to build its dedicated global set directly
    // rather than route through this constant.
    static constexpr uint32_t kUnboundedArrayDescriptorCapacity = 4096;

    // Builds one VkDescriptorSetLayout per descriptor set index used by
    // `layoutInfo.bindings` -- including an empty (zero-binding) layout for
    // any set index skipped by the shader (e.g. a shader using only set 0
    // and set 2 still needs a placeholder at index 1, since
    // vkCmdBindDescriptorSets addresses sets positionally by index, not by
    // the sparse set of numbers a shader happens to declare) -- then the
    // VkPipelineLayout tying them together with `layoutInfo.pushRanges`.
    //
    // A binding with `unboundedArray == true` gets
    // VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
    // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT (via
    // VkDescriptorSetLayoutBindingFlagsCreateInfo), and that set's layout
    // gets VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT --
    // both required by Vulkan validation for a set that will later be
    // written to via vkUpdateDescriptorSets while already bound (Task 3's
    // bindless table depends on this).
    //
    // Rejects (returns std::nullopt, logs via RX_LOG_ERROR) if the total
    // push-constant footprint (max over every range of offset+size) exceeds
    // the 128-byte guaranteed-minimum floor every Vulkan 1.0+ implementation
    // must support [spec Fixed decision #5, R:B2] -- enforced here, not in
    // reflect(), since reflection must faithfully report what a shader
    // actually declares even when that exceeds this engine's own budget
    // policy (see rx_shader's reflection_test.cpp for the "reflection
    // succeeds regardless" half of this contract). Also returns
    // std::nullopt if any underlying vkCreateDescriptorSetLayout/
    // vkCreatePipelineLayout call fails, cleaning up every handle already
    // created in that same call first.
    static std::optional<PipelineLayoutBundle> build(VkDevice device, const rx::shader::ShaderLayoutInfo& layoutInfo);
};

}  // namespace rx::rhi
