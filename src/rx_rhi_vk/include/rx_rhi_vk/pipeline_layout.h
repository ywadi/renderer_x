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
// themselves -- this bundle owns both, WITH ONE DOCUMENTED EXCEPTION: when
// `build()` was called with a non-null `externalSet0` (see that function's
// comment), `setLayouts[0]` is that caller-supplied handle, not one this
// bundle created -- this bundle never destroys it (on destruction or
// move-assignment) and the original caller remains solely responsible for
// its lifetime, which by construction outlives every use of it here (Task
// 6's BindlessTable instance, e.g., outlives every pipeline built against
// its `descriptorSetLayout()`). Every OTHER entry in `setLayouts` (index 1+
// always, index 0 too when no external layout was supplied) is owned and
// destroyed by this bundle exactly as before.
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

    // True iff setLayouts[0] is the caller-supplied `externalSet0` handle
    // from build() -- see the class comment above. destroyAll() skips
    // index 0 when this is true.
    bool externalSet0_ = false;
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
    //
    // EXTERNAL SET-0 SUBSTITUTION -- added this task, closing the gap Task
    // 3's review found: this builder used to always create its own set-0
    // layout from `layoutInfo` alone, sized to `kUnboundedArrayDescriptorCapacity`
    // with no VARIABLE_DESCRIPTOR_COUNT flag -- structurally incompatible
    // with rx::rhi::BindlessTable's real set-0 layout (built from caller
    // capacities, VARIABLE_DESCRIPTOR_COUNT on its last binding). A shader
    // reflecting a bindless-shaped set 0 (unbounded Texture2D/SamplerState/
    // StructuredBuffer arrays at bindings 0/1/2 -- exactly BindlessTable's
    // own fixed scheme, `BindlessTable::kSampledImageBinding` etc.) needs
    // its VkPipelineLayout's set 0 to be the ACTUAL BindlessTable it will
    // bind at draw time, not a lookalike this builder invented.
    //
    // When `externalSet0` is non-null: this builder does NOT create a set-0
    // layout at all -- `externalSet0` becomes `setLayouts[0]` verbatim,
    // NOT owned by the returned bundle (see PipelineLayoutBundle's own
    // comment: the caller, e.g. a BindlessTable that outlives every
    // pipeline built against it, remains solely responsible for destroying
    // it). Before doing so, this still validates that `layoutInfo`'s own
    // set-0 bindings (if any) are a subset-compatible SHAPE for that
    // substitution to be sound: every set-0 binding's number must be one of
    // BindlessTable's three fixed slots (kSampledImageBinding/
    // kSamplerBinding/kStorageBufferBinding) with EXACTLY that slot's
    // descriptor type (a mismatch -- e.g. a shader declaring a
    // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER at binding 0, where the bindless
    // table's binding 0 is always VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE -- is
    // rejected), and any binding declaring a bounded (non-unbounded) count
    // must stay within `kUnboundedArrayDescriptorCapacity` (this builder
    // cannot see the real external layout's actual per-binding capacity
    // through an opaque VkDescriptorSetLayout handle -- Vulkan exposes no
    // "describe this layout back to me" API -- so this is a generic sanity
    // ceiling, not a check against BindlessTable's specific instance
    // capacities). A shader is free to use a strict subset of the three
    // slots (e.g. images + samplers only, no storage buffers) -- that is
    // exactly the "subset-compatible" the brief asks for, not an exact-set
    // match. This is a host-side, pre-pipeline-creation sanity gate with a
    // clear logged reason on failure; it does not replace (and is narrower
    // than) whatever `vkCreateGraphicsPipelines` validation itself checks
    // against the real bound descriptor set's SPIR-V-declared shape.
    //
    // Set indices 1+ (and set 0 too, when `externalSet0` is
    // VK_NULL_HANDLE, the default) are entirely unaffected -- built exactly
    // as before.
    static std::optional<PipelineLayoutBundle> build(VkDevice device, const rx::shader::ShaderLayoutInfo& layoutInfo,
                                                       VkDescriptorSetLayout externalSet0 = VK_NULL_HANDLE);
};

}  // namespace rx::rhi
