#pragma once
// instance.h -- the material-INSTANCE half of rx_material [Phase 3 Task 7
// brief]: the reflected per-field parameter description a material
// instance's blob is laid out by (MaterialParamKind/MaterialParamInfo,
// collapsing Task 6's own second, api_impl.cpp-private reflection session
// into MaterialSystem's authoritative one -- see material_system.cpp's
// reflectMaterialLayout() and api_impl.cpp's own updated header comment),
// the caller-facing shape MaterialSystem::bindInstance() binds against
// (InstanceBinding), and the GPU-side machinery that turns a raw parameter
// blob into a real bound VkDescriptorSet (ParamArena).
//
// volk.h (not vulkan/vulkan.h) -- matching material_system.h's own
// convention (see that header's comment): every Vulkan type below is an
// opaque handle, this header itself never calls a raw vkFoo.
#include <volk.h>

#include <rx_core/handle.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/descriptor_arena.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rx::material {

// Deliberately re-declared here rather than pulled in via material_system.h
// -- avoids a circular #include (material_system.h itself includes THIS
// header, for MaterialParamInfo/InstanceBinding/ParamArena below, which in
// turn need to name a loaded material -- exactly MaterialHandle).
// Redeclaring the SAME alias to the SAME underlying type
// (`rx::core::Handle<struct MaterialTag>`) in two headers is legal C++ (a
// using-alias may be redeclared identically, the same rule that lets a
// typedef be repeated) and resolves to one identical type either way, since
// `struct MaterialTag` names the same incomplete tag in both -- matching
// this codebase's own established precedent of duplicating small, canonical
// declarations across translation units rather than forcing an awkward
// dependency edge (see material_system.cpp's own FNV-1a comment for the
// identical reasoning applied to a different kind of duplication).
using MaterialHandle = rx::core::Handle<struct MaterialTag>;

// The shape of a single reflected `TParams` field this engine's public
// setFloat/setFloat4/setTexture surface (rx_api.h) can bind against --
// classified the same way Task 6's now-removed second reflection session
// did (api_impl.cpp), just computed once here from MaterialSystem's own
// already-linked program instead of a throwaway duplicate. `Unsupported`
// covers every other real field shape (float3, matrices, ints, ...) with no
// matching setter yet: a lookup that finds an Unsupported field is a real,
// named parameter with the WRONG type for whichever setter was called
// (RX_E_INVALIDARG), not an unknown one (RX_E_NOTFOUND).
enum class MaterialParamKind : uint8_t { Float, Float4, TextureIndex, Unsupported };

// One reflected `TParams` field: its name, shape, and byte OFFSET/SIZE
// within the parameter block (Slang's Uniform parameter category) -- the
// offset/size Task 6's own reflection never computed (it only needed
// name/kind for setter validation; Task 7 needs offset/size too, to know
// WHERE in the instance's CPU-side blob a given field's bytes live so
// bindInstance() can copy that whole blob straight into a real UBO).
struct MaterialParamInfo {
    std::string name;
    MaterialParamKind kind = MaterialParamKind::Unsupported;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// The caller-facing shape MaterialSystem::bindInstance() (material_system.h)
// binds against -- deliberately NOT a pointer to any api_impl.cpp-private
// type (IRxMaterialInstance's own implementation, MaterialInstanceImpl,
// lives in a different translation unit and is not part of this library's
// internal public surface): any caller holding a real MaterialHandle plus
// exactly `paramSize` bytes laid out per that material's own
// materialParams() offsets can drive a bind, whether that caller is
// api_impl.cpp (bridging an IRxMaterialInstance's own blob -- see
// rx_api_detail.h's materialInstanceBlob()) or a future direct internal
// consumer. `paramData`/`paramSize` are never retained past the
// bindInstance() call that receives them -- the bytes are copied into the
// per-frame arena immediately.
struct InstanceBinding {
    MaterialHandle material;
    const void* paramData = nullptr;
    size_t paramSize = 0;
};

class ParamArena;

namespace detail {

// Test-only seam -- NOT part of the stable internal contract, mirroring
// rx_graph's own `detail::debugLastFrameFinalStages()` and this same
// library's own `detail::debugCompileCount()` carve-out convention:
// exposed purely so a standalone ParamArena test [Fix round 1,
// task-7-review.md F2] can read back the raw bytes writeAndAllocate()
// wrote into a given frame-in-flight slot's own host-visible buffer, to
// prove cross-frame isolation and reset behavior directly -- there is no
// other way to observe this from outside ParamArena's own implementation
// (writeAndAllocate() returns only an opaque VkDescriptorSet, by design;
// see that method's own comment). Returns nullptr if `arena` has no
// buffers at all (never true for a successfully create()'d ParamArena).
[[nodiscard]] const void* debugFrameBufferData(const ParamArena& arena, uint32_t frameIndex);

}  // namespace detail

// ParamArena -- the GPU-visible half of "material instance" [Task 7
// brief]: a per-frames-in-flight, host-visible, persistently-mapped,
// bump-allocated uniform-buffer arena (the CPU-writable byte storage) paired
// with an rx::rhi::DescriptorArena (the VkDescriptorSet-allocation half).
// Together this is exactly the brief's "per-frame host-visible arena
// buffer... allocate set-1 from DescriptorArena, write UBO descriptor"
// mechanism. Lives at namespace scope (not nested inside
// MaterialSystem::Impl) so it can be unit-tested on its own
// (test_material_system.cpp), mirroring rx_rhi_vk's own convention of
// giving each reusable piece (DescriptorArena, BindlessTable, FrameSync)
// its own test file -- this one lives in rx_material's own GPU test binary
// instead of rx_rhi_vk's, since it is rx_material's own composition of
// DescriptorArena plus a material-instance-shaped host buffer, not a
// reusable RHI primitive in its own right.
//
// FRAMES-IN-FLIGHT DISCIPLINE (binding global constraint, restated here
// where it is actually enforced): a blob written for frame N's draw must
// never corrupt frame N-1's still-in-flight draw. Bump-allocating into the
// CURRENT frame's own buffer/pool slot (selected by the most recent
// beginFrame() call) -- never the slot any OTHER frame-in-flight index
// owns -- is what gives this: two frames in flight can never share a
// buffer/pool slot at the same time, by construction, since
// beginFrame(frameIndex) picks slot `frameIndex % framesInFlight()` and a
// caller only advances to a new frameIndex after confirming (via its own
// fence wait) that the PREVIOUS occupant of that same slot has finished on
// the GPU -- exactly rx::rhi::FrameSync's/DescriptorArena's own established
// per-frame-in-flight reuse contract, applied here to a byte arena instead
// of Vulkan objects.
class ParamArena {
public:
    ParamArena(ParamArena&&) noexcept = default;
    ParamArena& operator=(ParamArena&&) noexcept = default;
    ParamArena(const ParamArena&) = delete;
    ParamArena& operator=(const ParamArena&) = delete;
    ~ParamArena() = default;

    // Bytes reserved per frame-in-flight slot for bump-allocated parameter
    // blobs -- 1 MiB, generous for Phase 3's sample scale (materials with
    // small (well under 256-byte) TParams blocks; hundreds of
    // bindInstance() calls per frame would fit comfortably). Documented,
    // fixed constant, not a runtime-tunable parameter -- a future phase
    // with materially larger scenes revisits this alongside
    // kMaxInstancesPerFrame below.
    static constexpr VkDeviceSize kBytesPerFrame = 1u * 1024u * 1024u;

    // This arena's own per-frame VkDescriptorSet ceiling -- one
    // bindInstance() call allocates exactly one set, so this is also the
    // maximum number of material-instance binds any one frame can issue
    // before writeAndAllocate() starts returning VK_NULL_HANDLE (logged,
    // never a crash).
    static constexpr uint32_t kMaxInstancesPerFrame = 512;

    // Each bump-allocation's start offset is rounded up to this alignment
    // before use -- 256 bytes is the Vulkan spec's own guaranteed UPPER
    // BOUND on `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment`
    // (every conformant implementation's real requirement is <= this), so
    // aligning to it unconditionally is always sufficient for
    // VkDescriptorBufferInfo::offset's alignment rule regardless of which
    // GPU this runs on -- the same well-known "align UBO sub-allocations to
    // 256" pattern used throughout the Vulkan ecosystem, not a
    // this-project-specific guess.
    static constexpr VkDeviceSize kUniformBufferAlignment = 256;

    static std::optional<ParamArena> create(VkDevice device, rx::rhi::Allocator& allocator, uint32_t framesInFlight);

    // Resets frame slot `frameIndex % framesInFlight()` for reuse: rewinds
    // that slot's bump-allocation cursor to 0 and resets its
    // DescriptorArena pool (rx::rhi::DescriptorArena::beginFrame()). Same
    // SAFETY CONTRACT as that method: only call once the caller has
    // confirmed (via its own fence wait) that slot's prior GPU work is
    // complete.
    void beginFrame(uint32_t frameIndex);

    // Bump-allocates `size` bytes (aligned per kUniformBufferAlignment) in
    // the CURRENT frame's host-visible buffer, copies `data` into them and
    // flushes them (correct and cheap even on an already-coherent memory
    // type -- see rx::rhi::Buffer::flush()'s own comment), allocates a
    // VkDescriptorSet of `setLayout` from the internal DescriptorArena, and
    // writes binding 0 of that set as a VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    // pointing at exactly the bytes just written. Returns VK_NULL_HANDLE
    // (logged) if `size` is 0, if the current frame's byte arena cannot fit
    // `size` more bytes, or if the DescriptorArena allocation itself fails
    // (e.g. kMaxInstancesPerFrame already exhausted this frame) -- callers
    // must not bind/draw with a VK_NULL_HANDLE result.
    [[nodiscard]] VkDescriptorSet writeAndAllocate(VkDescriptorSetLayout setLayout, const void* data, size_t size);

    [[nodiscard]] uint32_t framesInFlight() const { return static_cast<uint32_t>(buffers_.size()); }

private:
    friend const void* detail::debugFrameBufferData(const ParamArena& arena, uint32_t frameIndex);

    ParamArena() = default;

    // std::optional, not a plain member: rx::rhi::DescriptorArena's own
    // default constructor is private (create() is the only way to build a
    // real one -- see descriptor_arena.h's own comment), so ParamArena (a
    // different class) has no accessible default to construct it with
    // directly -- the same reason Impl (material_system.cpp) wraps its own
    // Allocator/Uploader/ParamArena members in std::optional too.
    VkDevice device_ = VK_NULL_HANDLE;
    std::optional<rx::rhi::DescriptorArena> descriptorArena_;
    std::vector<rx::rhi::Buffer> buffers_;  // one per frame-in-flight slot.
    std::vector<VkDeviceSize> cursors_;     // this slot's next free (unaligned) byte offset.
    uint32_t currentFrame_ = 0;
};

}  // namespace rx::material
