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

// [D29, gate ruling RC2, 2026-08-18] Which depth convention a depth/
// depth-stencil AttachmentDesc's pass expects -- Standard (near=0, far=1,
// VK_COMPARE_OP_LESS family, clear=1.0) is Vulkan's/this codebase's
// original, pre-D13 default; Reversed (near=1, far=0,
// VK_COMPARE_OP_GREATER_OR_EQUAL family, clear=0.0) is D13's main-camera
// convention. Meaningless (left at its default, Standard) for a
// color-only AttachmentDesc -- Executor only ever reads this field off a
// pass's DEPTH attachment (see executor.cpp's two clear-value sites,
// design doc D29's own citation: executor.cpp:646 and :1119 as of the
// decision's own writing).
//
// Why a field on AttachmentDesc, not a second PassSignature/Pass axis: the
// clear value AND the pass's own expected compare direction both derive
// from the SAME one bit of information (which way "closer" reads on this
// attachment) -- D29's own text: "the clear value and the pass's expected
// compare direction derive from it". `PassSignature` (pass_signature.h)
// deliberately stays attachment-SHAPE-only (format/count/samples) and this
// convention is not part of it, matching that header's own documented
// scope; a pass's fixed-function depth-compare-op is a `VkPipeline`-level
// concern (D28, MaterialSystem::getPipeline()), entirely separate from
// what this field governs (Executor's own clear-value selection).
//
// Default is Standard -- BYTE-IDENTICAL to this field's absence pre-D29:
// every existing Pass::setDepthStencilOutput() call across every sample/
// test in this codebase constructs an AttachmentDesc without ever naming
// this field, and gets the exact clear value (1.0) Executor has always
// used, unchanged.
enum class DepthConvention : uint8_t { Standard, Reversed };

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

    // [D29] See DepthConvention's own comment above. Ignored for a
    // color-only attachment.
    DepthConvention depthConvention = DepthConvention::Standard;
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

// [Task 2, gate ruling RC2] One storage-IMAGE resource's declared shape, as
// written by the pass that establishes it (Pass::addStorageImageOutput()) --
// mirrors BufferDesc's own spirit (a plain declared-shape struct alongside
// AttachmentDesc/BufferDesc) generalized with the mip/array-layer/cube
// fields neither of those two carries: AttachmentDesc-backed resources
// (color/depth/history) stay single-mip/single-layer in this task (see
// PhysicalResource::mipLevels/arrayLayers/cube's own comment -- rasterized
// attachment output growing subresource addressing is explicitly out of
// this task's scope, since no named Phase 5 consumer needs it yet), so only
// a storage-image resource can ever have more than one mip/layer today.
//
// `cube` requires `arrayLayers` to be a positive multiple of 6
// [VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT's own Vulkan-spec precondition,
// VUID-VkImageCreateInfo-flags-08865] -- RenderGraph::compile() validates
// this at the declaring pass and throws, naming the resource, otherwise
// (render_graph.cpp).
struct ImageDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    SizeClass sizeClass = SizeClass::SwapchainRelative;
    float width = 1.0F;
    float height = 1.0F;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    bool cube = false;
};

// [Task 2, gate ruling RC2] Sentinel meaning "every remaining mip level
// from baseMipLevel" / "every remaining array layer from baseArrayLayer" --
// mirrors VK_REMAINING_MIP_LEVELS/VK_REMAINING_ARRAY_LAYERS's own semantics
// without pulling either literal into a device-free struct's default
// member initializer (this header stays Vulkan-Headers-only, no volk --
// see the file's own top comment -- and VK_REMAINING_MIP_LEVELS/
// VK_REMAINING_ARRAY_LAYERS are plain `(~0U)` `#define`s in
// vulkan_core.h anyway, so this is simply naming that same value locally
// rather than depending on the macro).
inline constexpr uint32_t kRemainingMipLevels = ~0U;
inline constexpr uint32_t kRemainingArrayLayers = ~0U;

// [Task 2, gate ruling RC2] One image resource's declared subresource
// range -- the mip level(s) + array layer(s) a single Pass::Declaration
// addresses. Default-constructed means "the whole resource" (every mip,
// every layer), BYTE-IDENTICAL in effect to every declaration kind that
// predates this task (addColorOutput/setDepthStencilOutput/addTextureInput/
// addHistoryInput/setHistoryOutput), none of which ever specify one: every
// PhysicalResource those kinds establish has exactly one mip and one layer
// (see PhysicalResource::mipLevels/arrayLayers's own comment), so "the
// whole resource" and "mip 0, layer 0 alone" resolve to the exact same
// single subresource for them either way -- this struct changes nothing
// about their existing behavior.
//
// RenderGraph::compile() resolves the two sentinel fields (levelCount/
// layerCount) into concrete counts once a declaration's target resource's
// real mipLevels/arrayLayers are known (render_graph.cpp step 4) -- every
// ResourceAccess::subresource a compiled graph exposes is fully resolved
// (no sentinel values survive compile()).
struct Subresource {
    uint32_t baseMipLevel = 0;
    uint32_t levelCount = kRemainingMipLevels;
    uint32_t baseArrayLayer = 0;
    uint32_t layerCount = kRemainingArrayLayers;

    bool operator==(const Subresource&) const = default;
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

    // [Task 2, gate ruling RC2] The RESOLVED (no sentinel values -- see
    // Subresource's own comment) subresource range this declaration
    // addresses. Meaningless (left default-constructed, i.e. "the whole
    // resource") for a buffer access or for any declaration kind that
    // predates this task -- populated with a real, possibly-narrower range
    // only for a StorageImageOutput/StorageImageInput/TextureInput
    // declaration that named an explicit `subresource` argument.
    Subresource subresource;
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

    // [Task 2, gate ruling RC2] Real image shape -- meaningless (left at
    // their defaults, 1/1/false) for a buffer resource. Every
    // AttachmentDesc-backed resource (color/depth/history/texture-input-
    // only) stays 1/1/false: this task does not extend rasterized
    // attachment output to carry more than one mip/layer (see ImageDesc's
    // own comment for why that is a deliberate scope boundary, not an
    // oversight). Only a resource established via addStorageImageOutput()
    // ever has mipLevels/arrayLayers/cube set from its declared ImageDesc.
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    bool cube = false;

    // Phase 4 Task 1: true for a resource established via
    // Pass::setHistoryOutput()/read via Pass::addHistoryInput() -- i.e. a
    // persistent, ping-ponged resource backed by TWO pinned physical
    // images (Executor's pinned pool, transient_pool.h), never the regular
    // discard-per-frame transient pool a normal PhysicalResource uses.
    // Mutually exclusive with a name ever appearing in a non-history
    // declaration (RenderGraph::compile()'s own namespace-mixing
    // validation rejects a graph that tries both) and with isBackbuffer
    // (a history name can never satisfy setBackbufferSource()'s own
    // writer-existence check -- see render_graph.cpp's comment on why
    // Pass::isWriteKind() deliberately excludes HistoryOutput). firstUsePass/
    // lastUsePass/imageUsage are still populated the same generic way as
    // any other resource but are largely informational for a history
    // resource -- ping-pong slot selection and cross-frame layout/stage
    // tracking are entirely Executor-side concerns (executor.cpp), not
    // something this device-free compile() result drives directly.
    bool isHistory = false;
};

}  // namespace rx::graph
