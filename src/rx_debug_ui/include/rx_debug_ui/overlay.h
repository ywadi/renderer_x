#pragma once
// Thread-affinity (D5, Phase 4): main-thread-only, in full -- `Overlay`
// wraps a single, PROCESS-GLOBAL `ImGuiContext` (Dear ImGui is single-
// threaded by design: one global context, no internal synchronization,
// well-established public knowledge of the library -- gate matrix-issue16
// row 13). `create()`, `processEvent()`, `beginFrame()`, and the graph
// pass's own execute callback (installed by `addPass()`) must all run on
// the SAME thread that called `create()`. At most one live `Overlay`
// should exist per process for the same reason (a second concurrently-live
// instance would silently share/stomp the first one's global ImGui
// context) -- this is a documented usage contract, not something this
// class enforces at runtime.
//
// FONT/TEXTURE UPLOAD -- DOCUMENTED D25/D24 EXCEPTION [gate ruling #16,
// 2026-08-18, BINDING]: this class's font atlas (and any later-registered
// HUD texture, e.g. via ImGui_ImplVulkan_AddTexture) does NOT go through
// rx::rhi::Uploader/UploadTicket -- unlike every Stage-1 call site D25
// lists (GeometryPool upload, TextureCache load, importer, async import).
// It structurally cannot: the vendored, unmodified Vulkan backend
// (imgui_impl_vulkan.cpp's ImGui_ImplVulkan_UpdateTexture(), called
// internally from ImGui_ImplVulkan_RenderDrawData()) does its OWN raw
// vkCreateImage/vkAllocateMemory/vkCreateBuffer calls -- entirely
// bypassing VMA -- on its own internal one-shot command pool, and BLOCKS
// the graphics queue with its own `vkQueueWaitIdle` (imgui_impl_vulkan.cpp,
// upstream's own "// FIXME-OPT: Suboptimal!" comment) until that upload
// completes. This project's "prefer unmodified vendored deps" posture
// (Global Constraints) and the header's own "You can use unmodified
// imgui_impl_* files in your project" comment both argue against patching
// it to route through Uploader instead. Two consequences, both load-
// bearing:
//   (a) `create()` FORCES this upload to happen once, synchronously,
//       during construction (before any real per-frame rendering begins)
//       so the one unavoidable full-queue stall this vendored path incurs
//       is a one-time startup cost, not a steady-state one -- see
//       overlay.cpp's own comment at the call site for exactly how.
//   (b) The font atlas (and any HUD textures registered once, near
//       startup) are created ONCE; a HUD design that later causes
//       per-frame texture churn (e.g. a dynamically-regenerated icon
//       atlas) would silently reintroduce this same full-queue stall on
//       every affected frame -- rx_debug_ui's own GPU test asserts
//       `vkQueueWaitIdle` fires AT MOST ONCE across a fixed N-frame
//       steady-state run specifically to catch that regression class.
//   (c) This class's own raw Vulkan allocations (the font atlas image +
//       memory, the vertex/index buffer rotation ImGui_ImplVulkan_
//       RenderDrawData() manages internally) are INVISIBLE to D24's
//       per-category VMA/vmaGetHeapBudgets() accounting
//       (rx_rhi_vk/memory_report.h) -- see that header's own comment for
//       the matching note on the report's documented scope. Bounded and
//       small (a font atlas + a handful of HUD textures, never scene-scale
//       data), not a correctness gap, but a real blind spot a reader of
//       the memory report should not mistake for exhaustive coverage.
#include <SDL3/SDL.h>  // SDL_Event -- matches rx_platform/window.h's own direct-include convention (input.h is deliberately SDL-free).
#include <vulkan/vulkan_core.h>

#include <optional>
#include <string_view>

namespace rx::rhi {
class Device;
}  // namespace rx::rhi

namespace rx::platform {
class Window;
}  // namespace rx::platform

namespace rx::graph {
class RenderGraph;
}  // namespace rx::graph

namespace rx::debug_ui {

// Dear ImGui v1.92.9b [spec D20, gate ruling #16] as a normal declared
// render-graph pass. Vendored (third_party/CMakeLists.txt) core + sdl3 +
// vulkan backends only -- no docking/multi-viewport (the plain, non-
// `-docking` tag structurally excludes that whole code path, not merely a
// runtime flag discipline). This is the ONLY public seam through which
// ImGui symbols are meant to reach a consumer outside this module and
// samples -- every rx_* CORE library's link closure is asserted free of
// the vendored `imgui` target at CMake configure time
// (cmake/DependencyBoundaryCheck.cmake, called from the root
// CMakeLists.txt) [gate ruling #16 row 12].
class Overlay {
public:
    Overlay(Overlay&&) noexcept;
    Overlay& operator=(Overlay&&) noexcept;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // See the class-level comment above: the caller must have already
    // brought `device` to a real idle point (no in-flight frame still
    // referencing this overlay's pipeline/descriptor pool/font texture)
    // before this runs -- the SAME documented contract
    // rx::rhi::FrameSync's own destructor states, not a new convention.
    ~Overlay();

    // Builds the overlay against `device`/`window`, targeting `colorFormat`
    // (the render-graph target's own attachment format this overlay's pass
    // will write -- typically `device.swapchainFormat()`) via
    // UseDynamicRendering. Order of operations, all inside this one call:
    // creates this overlay's OWN VkDescriptorPool (two typed pool-size
    // entries -- VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE and
    // VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_POOL_CREATE_FREE_
    // DESCRIPTOR_SET_BIT set -- sized per the vendored backend's own
    // stated minimums with headroom for HUD textures beyond the font atlas
    // alone, see overlay.cpp); calls ImGui::CreateContext(); calls
    // `ImGui_ImplSDL3_InitForVulkan(window.sdlWindow())` THEN
    // `ImGui_ImplVulkan_Init(...)` (platform backend before renderer
    // backend, matching every upstream ImGui example's own ordering);
    // immediately calls `ImGui_ImplSDL3_SetGamepadMode(Manual, nullptr, 0)`
    // (kills the backend's own default AutoFirst gamepad-ownership race
    // against rx_platform::Window's own gamepad lifecycle -- gate ruling
    // #16 row 7; gamepad HUD navigation is deliberately NOT enabled this
    // phase as a direct consequence); forces the font atlas's one-time GPU
    // upload synchronously (see class comment's FONT/TEXTURE UPLOAD
    // section). Returns std::nullopt (logged via RX_LOG_ERROR) on any
    // failure -- including a deliberately-invalid caller-supplied
    // configuration this function validates itself before ever calling
    // into the vendored backend (see overlay.cpp: the vendored backend's
    // own `IM_ASSERT` preconditions compile to nothing in this project's
    // actual NDEBUG-defined RelWithDebInfo builds -- both this project's
    // presets -- so relying on them alone would silently let a bad config
    // proceed; this function's own checks are what actually make "fail
    // loudly, not silently proceed" true in the builds this project ships).
    static std::optional<Overlay> create(rx::rhi::Device& device, rx::platform::Window& window, VkFormat colorFormat);

    // The event-dispatch seam [gate ruling #16]: wraps
    // `ImGui_ImplSDL3_ProcessEvent(&event)`. Pass this directly as
    // `rx::platform::Window::pumpEvents()`'s `preDispatch` argument (the
    // seam that method gained specifically for this -- see its own header
    // comment), or call it yourself once per drained SDL event, BEFORE any
    // other code this frame reads `ImGui::GetIO().WantCaptureMouse`/
    // `WantCaptureKeyboard` (gate ruling #14/#16: those flags must already
    // be current by the time platform-input accumulation gates on them).
    void processEvent(const SDL_Event& event);

    // Frame-start sequencing: `ImGui_ImplVulkan_NewFrame()` +
    // `ImGui_ImplSDL3_NewFrame()` (both backend NewFrames, in that order)
    // + `ImGui::NewFrame()` (core, last) -- the exact ordering every
    // upstream ImGui example uses. Call once per frame, before any UI-
    // building code (including whatever the graph pass's own execute
    // callback, installed by addPass() below, will end up rendering).
    void beginFrame();

    // Declares this overlay's render-graph pass against `graph`: a
    // side-effect pass (rx_graph::Pass::setSideEffect() -- no downstream
    // consumer reads its output, so it would otherwise be culled) with no
    // resource reads (ImGui reads its own internal draw-data state, not a
    // graph-tracked resource) and one color output on `targetName`. LOAD-
    // not-CLEAR is a named acceptance criterion [gate ruling #16]: whether
    // this pass's own attachment load-op is LOAD or CLEAR is NOT a
    // parameter this method exposes -- rx_graph::Executor derives it
    // automatically from whether an EARLIER pass in the same frame already
    // wrote `targetName` (LOAD) or this is that name's first write this
    // frame (CLEAR) -- so a caller that wants the underlying scene to
    // remain visible under the HUD simply declares its own scene pass(es)
    // against the SAME `targetName` BEFORE calling this method each frame;
    // getting that ordering wrong (declaring this pass first, or against a
    // name nothing else writes) silently blanks the scene under the HUD
    // rather than failing loudly, so callers must get the ordering right,
    // not merely call this method somewhere. The execute callback records
    // `ImGui::Render()` then `ImGui_ImplVulkan_RenderDrawData(ImGui::
    // GetDrawData(), ctx.cmd)` into the pass's own command buffer -- no
    // separate `ImGui::EndFrame()` call is needed (`Render()` calls it
    // internally, per ImGui's own documented contract). Call once per
    // frame, after beginFrame() and every UI-building call.
    void addPass(rx::graph::RenderGraph& graph, std::string_view targetName);

private:
    // Plain-member RAII, no PImpl: mirrors every other GPU-resource-owning
    // class in this codebase (rx::platform::Window, rx::rhi::Device/
    // Uploader) rather than introducing a new pattern for this one class.
    // Everything ELSE this class touches (the ImGui backends' own init
    // state, the font atlas, the vertex/index buffer rotation) is owned by
    // Dear ImGui's own global/static state, not by this object -- `device_`
    // doubling as the "do I own a live context to tear down" sentinel
    // (VK_NULL_HANDLE after a move-from or default-construction, matching
    // Window's own `window_`-as-sentinel convention) is sufficient.
    explicit Overlay(VkDevice device, VkDescriptorPool descriptorPool, VkFormat colorFormat)
        : device_(device), descriptorPool_(descriptorPool), colorFormat_(colorFormat) {}

    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    // The format `create()` was called with -- re-supplied to addColorOutput()
    // below rather than re-derived: RenderGraph::compile() takes the LAST
    // (execution-order) ColorOutput declaration's own AttachmentDesc as a
    // resource's canonical shape (render_graph.cpp: `resource.attachment =
    // decl.attachment;`, unconditionally overwritten by every writer) --
    // since this overlay's own pass is meant to be declared AFTER the
    // scene pass(es) that establish `targetName` (see addPass()'s own
    // comment), this class's own declaration is exactly the one whose
    // AttachmentDesc "wins" that overwrite, so it must describe the SAME
    // shape the earlier pass(es) already established, not an arbitrary
    // one. A NON-backbuffer `targetName` sized differently than the
    // swapchain (SizeClass::Absolute, or a non-1.0 SwapchainRelative
    // multiplier) is outside this task's scope -- addPass() always
    // declares SwapchainRelative 1.0x1.0 (resources.h's own stated common
    // case), which is exactly right for `targetName == backbufferSource`
    // regardless (RenderGraph::compile() overrides a backbuffer resource's
    // shape from the swapchain unconditionally, ignoring every writer's
    // own AttachmentDesc) and for any other same-size HUD target.
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
};

namespace detail {

// Test-only seam -- NOT part of the stable public contract [gate ruling
// #16 row 5, matrix's "at-most-once QueueWaitIdle" acceptance criterion].
// Mirrors rx::core::debug::detail::setViolationHookForTests's own carve-out
// convention (rx_core/debug_checks.h): a plain function pointer, never
// std::function, installed once before the code under test runs and
// restored to nullptr (the default -- real production behavior) once the
// test is done, typically via an RAII scope guard at the call site.
//
// Overlay::create() loads every Vulkan function the vendored ImGui backend
// calls through its OWN loader function (imguiVulkanFunctionLoader,
// overlay.cpp), rather than the direct-prototype/volk-global path most of
// this codebase's own Vulkan calls use -- see third_party/CMakeLists.txt's
// own comment on the vendored `imgui` target's VK_NO_PROTOTYPES choice for
// why. That loader is this class's OWN code, so it is the natural,
// zero-extra-machinery seam for a GPU test to intercept specifically
// "vkQueueWaitIdle" -- and ONLY that one symbol, resolved ONLY for the
// vendored ImGui backend's own internal, TU-local static function-pointer
// table, never the shared global `vkQueueWaitIdle` this project's own
// engine-side code (e.g. rx::rhi::CommandContext::runOnce()) calls through
// volk. Installing a non-null hook here means the NEXT
// Overlay::create() call's internal ImGui_ImplVulkan_LoadFunctions() call
// resolves "vkQueueWaitIdle" to `hook` instead of the real
// vkGetInstanceProcAddr()-resolved function; `hook` itself is responsible
// for forwarding to the real function (typically the same global
// `vkQueueWaitIdle` volk symbol every other Vulkan call in this project
// already uses) -- this seam only lets a test COUNT calls, never skip the
// real wait (skipping it would leave the font atlas upload genuinely
// incomplete when later code reads from it, a correctness bug, not a test
// convenience). `nullptr` (the default) restores the real,
// vkGetInstanceProcAddr()-resolved function for every symbol, including
// this one.
using QueueWaitIdleFn = VkResult (*)(VkQueue);
void setQueueWaitIdleHookForTests(QueueWaitIdleFn hook);

}  // namespace detail

}  // namespace rx::debug_ui
