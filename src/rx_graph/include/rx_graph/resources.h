#pragma once
// Vulkan-Headers only -- no volk.h, no vulkan/vulkan.h (which pulls in
// platform surface headers this file has no business depending on), and
// nothing from rx_rhi_vk. Task 1 is device-free by design [Task 1 brief]:
// every type below is a plain Vulkan *type* (enum/flags/handle-less struct),
// never a live handle a device created. Tasks 2-3 are the first things in
// rx_graph that touch a real VkDevice.
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>

namespace rx::graph {

// Scheduling hint carried on every declared pass. Phase 3's compile()/
// execute() maps every pass to the graphics queue regardless of this value
// [spec Phase 3 design, D4 -- async compute scheduling is an explicit,
// recorded deferral, not an oversight]. It exists now so the Pass API
// doesn't need an ABI/source break the day cross-queue scheduling lands;
// nothing in Task 1 reads it for anything but documentation -- compile()'s
// own pass-kind classification (see render_graph.cpp) is derived from
// declared attachment outputs, not from this hint.
enum class QueueClass : uint8_t {
    Graphics,
    AsyncCompute,
};

// How an attachment's pixel extent is specified. SwapchainRelative is the
// common case (a shadow map or HDR color target sized as a fraction of the
// window); Absolute is for a fixed-resolution target (e.g. a shadow atlas)
// that must not change size when the window is resized.
enum class SizeClass : uint8_t {
    SwapchainRelative,
    Absolute,
};

// One image resource's declared shape, as written by the pass that
// establishes it (addColorOutput/setDepthStencilOutput). `width`/`height`
// are a multiplier of RenderGraph::compile()'s CompileInfo::swapchainWidth/
// Height when `sizeClass` is SwapchainRelative (1.0F means "exactly the
// swapchain's own extent"), or literal pixel counts when Absolute.
//
// compile() resolves every non-buffer PhysicalResource's own copy of this
// struct to `sizeClass == Absolute` with `width`/`height` in real pixels --
// see PhysicalResource::attachment's comment -- but a Pass's *declared*
// AttachmentDesc (the argument passed to addColorOutput, still owned by the
// Pass itself) is never mutated; only the resolved copy compile() stores on
// PhysicalResource changes.
struct AttachmentDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    SizeClass sizeClass = SizeClass::SwapchainRelative;
    float width = 1.0F;
    float height = 1.0F;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// One buffer resource's declared shape, as written by the pass that
// establishes it (addStorageBufferOutput). Unlike AttachmentDesc's
// image-usage handling (PhysicalResource::imageUsage unions a flag per
// *kind* of declared access -- see that field's comment), `usage` here is
// taken verbatim from whichever write declared it: there is no per-kind
// derivation for buffers because VkBufferUsageFlags has no compile()-visible
// equivalent of "this kind of access always implies this usage bit" the way
// e.g. a texture-input read always implies VK_IMAGE_USAGE_SAMPLED_BIT.
struct BufferDesc {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
};

// One declared access to one physical resource, resolved by compile() from
// the *kind* of declaration (addColorOutput vs addTextureInput, etc.) per
// the Task 1 brief's compile algorithm step 1. `layout` is meaningless for
// buffer resources and left VK_IMAGE_LAYOUT_UNDEFINED there.
struct ResourceAccess {
    uint32_t physicalIndex;
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
    VkImageLayout layout;
};

// One physical resource compile() resolved from every declaration sharing
// its name, across every surviving (non-culled) pass. Phase 3 Task 1 does
// not version or alias resources: one declared name is exactly one
// PhysicalResource for the lifetime of a compiled graph [spec Phase 3
// design, D4 -- the lifetime range this struct carries is the *input* a
// future aliasing allocator needs, not an aliasing decision itself; Phase 3
// backs every PhysicalResource with its own pooled allocation].
struct PhysicalResource {
    std::string name;
    bool isBuffer = false;

    // Valid (isBuffer == false) or default-constructed (isBuffer == true).
    // Resolved to `sizeClass == Absolute`, real pixels, by compile() --
    // see AttachmentDesc's own comment.
    AttachmentDesc attachment;

    // Valid (isBuffer == true) or default-constructed (isBuffer == false).
    BufferDesc buffer;

    // Union, across every declared access (read or write) by any surviving
    // pass, of the VkImageUsageFlags bit that access kind implies (color
    // output -> COLOR_ATTACHMENT_BIT, depth output ->
    // DEPTH_STENCIL_ATTACHMENT_BIT, texture input -> SAMPLED_BIT). Left 0
    // for buffer resources -- VkImageUsageFlags has no buffer meaning.
    // Task 1's Pass API has no readback/screenshot declaration, so the
    // "+ TRANSFER_SRC for readback targets" case a future task may add is
    // not modeled here yet -- there is nothing to union it from.
    VkImageUsageFlags imageUsage = 0;

    // Positions into CompiledGraph::executionOrder() (NOT raw pass
    // indices) of this resource's first and last touching pass, over every
    // surviving pass that reads or writes it. Equal when a resource is
    // touched by exactly one surviving pass (e.g. the backbuffer, touched
    // only by its final writer).
    uint32_t firstUsePass = 0;
    uint32_t lastUsePass = 0;

    bool isBackbuffer = false;
};

}  // namespace rx::graph
