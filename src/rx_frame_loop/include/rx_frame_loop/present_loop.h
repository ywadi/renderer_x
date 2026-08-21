#pragma once
// rx_frame_loop/present_loop.h -- rx::frame_loop::PresentLoop, the shared
// present-loop helper [Phase 5 Task 5, ticket #41, gate ruling T5]: acquire
// -> status handling (NeedsRecreate/suspended-present/SurfaceLost) ->
// recreate-swapchain-and-dependents (incl. the graph-recompile trigger) ->
// present, with a frame-body callback -- closing the "nine independently
// hand-written present loops" pattern the scope-growth comment names as the
// source of this phase's recurring integration-bug class (descriptor
// sizing, per-FIF buffering, mouse capture, node transforms, surface-lost
// consumption).
//
// MODULE HOME (matrix-p5t05 Open Questions #3): a NEW, small, OPTIONAL
// engine module, layered exactly like `rx_debug_ui` (the established
// precedent already proving this shape is normal practice in this
// codebase -- `src/rx_debug_ui/CMakeLists.txt`:
// `target_link_libraries(rx_debug_ui PUBLIC imgui rx_platform rx_rhi_vk
// rx_graph rx_core)`, an optional engine module already layered above
// `rx_platform`+`rx_rhi_vk`+`rx_graph`). This satisfies every binding text
// at once: the plan's Global Constraint ("promote into the engine",
// `plan:88-93`), the phase ledger's "engine/SHARED facilities" phrasing
// (`progress.md:3`), and the toolchain/RHI design spec's Main-loop-
// ownership decision (`2026-08-09-toolchain-platform-rhi-design.md:500-508`
// -- "RendererX is library-model... no engine-owned loop... An OPTIONAL
// thin 'runner' convenience... built ON the library API and never required
// by it" -- this module IS exactly that convenience, seeded six phases
// early; see the phase ledger's own erratum note for the registry-line
// correction this seeding implies). `rx_graph`/`rx_rhi_vk`/`rx_platform`
// themselves gain NO new dependency and NO new required surface from this
// module's existence -- a host embedding RendererX that owns its own
// window/frame loop (the project's PRIMARY audience per the library-model
// decision) never needs to link this module at all.
//
// SCOPE, restated from the ticket's own framing: this module owns LOOP
// MECHANICS only -- fence/acquire/status-branch/recreate/submit/present/
// advance, and the swapchain-dependent resources every sample independently
// hand-rolled (per-swapchain-image VkImageViews, FrameSync, the compiled
// RenderGraph's realized state). It does NOT own: event pumping (SDL_Event
// dispatch stays the caller's job -- see `Window::pumpEvents()`), input
// handling/HUD content/scene updates (all sample-local, unpolluted), or
// *whether* to proactively recreate for a present-mode toggle/F11/live
// drag-resize (a UX policy decision the caller makes and then reports via
// `recreateAndDependents()` -- this module supplies the pure `f11Toggles-
// Fullscreen()`/`shouldRecreateForPixelSize()` DECISIONS, absorbed here
// from `samples/09_scene/window_resize.h` per matrix row 7, but the caller
// still decides WHEN to consult them).
//
// Thread-affinity (D5): create()/runFrame()/recreateAndDependents() are
// main-thread-only, matching every other present-loop-adjacent type in this
// codebase (`rx::rhi::Device`, `rx::rhi::FrameSync`) -- this class owns no
// synchronization of its own and performs no internal thread guard (a
// dev-time `RX_ASSERT_MAIN_THREAD` would need `rx_core/debug_checks.h`,
// deliberately not added here since neither `Device` nor `FrameSync`
// enforces it at their OWN call sites either -- see docs/threading.md).
#include <volk.h>

#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace rx::rhi {
class Allocator;
}  // namespace rx::rhi

namespace rx::frame_loop {

// [Phase 5 Task 5, ticket #41 row 7; promoted from
// samples/09_scene/window_resize.h] Pure, device-free decision functions
// the shared present loop's own orchestration (below) and a caller's own
// event-handling code both consult. Free functions, not PresentLoop
// methods, so they stay unit-testable with zero VkDevice/Window/RenderGraph
// dependency, exactly like their sample-local originals were.

// [Issue #36] `lastHandled` is the extent this run's own swapchain was last
// (re)built against (`PresentLoop::lastHandledExtent()`); `observed` is
// `rx::platform::Window::lastPixelSizeEvent()`'s current value. Returns
// true only when a recreation should be proactively triggered THIS frame --
// see the sample-local original's full derivation (preserved verbatim):
// `observed` with either dimension == 0 never triggers a recreation here
// (both because `lastPixelSizeEvent()` itself starts at {0,0} before any
// real event, and because window.h's own contract makes that accessor an
// optimization/logging signal only -- the AUTHORITATIVE suspended-present
// decision is always `Device::recreateSwapchain()`'s own live-queried
// guard, which a genuine minimize still reaches via the ordinary
// NeedsRecreate path).
[[nodiscard]] bool pixelSizeRequiresRecreate(VkExtent2D lastHandled, VkExtent2D observed);

// [Issue #36] A caller's own `pumpEvents()` `preDispatch` seam calls this on
// every real "F11 key down, not a repeat" event -- only when this returns
// true should the caller flip its own runtime fullscreen toggle, so a
// future keyboard-focused HUD widget can still claim F11 for itself without
// also toggling fullscreen underneath it. Deliberately its own named
// function rather than reusing `rx::platform::escTogglesCapture()` (same
// `!imguiWantsKeyboard` shape today) -- the two gate different call sites
// free to diverge independently later, exactly the reasoning the
// sample-local originals already established for this same coincidence.
[[nodiscard]] bool f11TogglesFullscreen(bool imguiWantsKeyboard);

// [Issue #74] Pure decision for a caller's own post-loop teardown: after
// the present loop has stopped (either gracefully, via
// `PresentLoop::isSurfaceLost()`, or on a hard failure), the caller's own
// (previously easy to leave unchecked) `vkDeviceWaitIdle()` result should
// be checked against this function before proceeding into fine-grained
// per-object teardown. True ONLY for the exact compound condition
// reproduced directly on real NVIDIA/Xcb hardware: `vkDeviceWaitIdle()`
// itself reports `VK_ERROR_DEVICE_LOST` AND the loop already knows the
// surface is lost (`surfaceLost` -- i.e. `PresentLoop::isSurfaceLost()`)
// -- a lost device in that exact combination does not actually wait for
// anything, so letting teardown proceed regardless produces real,
// unfiltered "still in use" validation errors (nothing was actually
// drained). A device lost while the surface is still alive is a DIFFERENT,
// more serious situation this function deliberately does not touch (stays
// a hard, caller-visible failure). See the original sample-local
// derivation (samples/09_scene/window_resize.h, Issue #74) for the full
// account -- verbatim logic, promoted unchanged.
[[nodiscard]] bool shouldSkipTeardownAfterDeviceLoss(VkResult waitIdleResult, bool surfaceLost);

// One rendered frame's worth of caller-visible state, valid only for the
// duration of the `FrameBodyFn` call it is passed to -- do not retain `cmd`
// past that call (`PresentLoop::runFrame()` calls `vkEndCommandBuffer()`
// immediately after `frameBody` returns).
struct FrameContext {
    // This frame's frame-in-flight slot (0..framesInFlight()-1) -- the SAME
    // index `Device::recreateSwapchain()`-adjacent per-FIF resources (e.g.
    // `rx::rhi::PerFrameStorageBuffer`) are written through, and the SAME
    // value `rx::material::MaterialSystem::beginFrame()`'s own
    // `frameInFlightIndex` parameter expects.
    uint32_t frameInFlightIndex = 0;
    // Monotonically increasing, never repeats for this PresentLoop's
    // lifetime -- mirrors `rx::rhi::FrameSync::frameNumber()` exactly (this
    // is that same value, read at the same point in the frame every
    // caller's own pre-promotion loop already read it at). The value
    // `MaterialSystem::beginFrame()`'s own `frameNumber` parameter expects.
    uint64_t frameNumber = 0;
    uint32_t imageIndex = 0;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    // Already `vkBeginCommandBuffer()`'d (ONE_TIME_SUBMIT) when `frameBody`
    // receives it -- record directly into it; `PresentLoop::runFrame()`
    // calls `vkEndCommandBuffer()` once `frameBody` returns.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

using FrameBodyFn = std::function<void(const FrameContext&)>;

// Optional SECOND callback [the scope-growth comment's own "frame-body
// callback(s)", plural]: invoked once per REAL swapchain recreation (never
// while suspended, and never on a recreation that observed surface loss) --
// lets a caller rebuild its own per-swapchain-extent resources that live
// OUTSIDE any RenderGraph this loop was constructed with (e.g. a
// hand-rolled depth buffer, samples 03_bindless_mesh/04_streaming's own
// pattern -- neither uses a RenderGraph, so their depth attachment is not a
// graph-managed transient this class already rebuilds via
// `Executor::realize()`). Receives the new swapchain extent
// (`Device::swapchainExtent()`, already live by the time this runs).
// Returns false to signal a hard failure (propagated as
// `Result::Failed` from whichever call -- `runFrame()` or
// `recreateAndDependents()` -- triggered this recreation). Called AFTER
// this class's own swapchain views/FrameSync are rebuilt but BEFORE any
// RenderGraph recompile/realize step, matching the relative ordering every
// pre-promotion sample that had both used.
using OnRecreateFn = std::function<bool(VkExtent2D newExtent)>;

// The status every present-loop-driving call in this class can resolve to
// -- deliberately the SAME four-way shape for both `runFrame()` and
// `recreateAndDependents()` (`Skipped` is simply never returned by the
// latter), so a caller composes them with one shared switch/if-chain
// instead of two different result vocabularies.
enum class Result {
    // A real frame was recorded, submitted, and presented (runFrame()), or
    // a recreation completed without a hard failure or newly-detected
    // surface loss (recreateAndDependents()) -- the caller should continue
    // its own loop.
    Ok,
    // runFrame() ONLY: no real frame was recorded this call (a Suspended
    // retry, or a NeedsRecreate this call absorbed) -- `frameBody` was NOT
    // invoked. The caller should simply call runFrame() again next
    // iteration; no special handling is required.
    Skipped,
    // A hard, unrecoverable failure (a real Vulkan call failed, or a
    // genuine DeviceLost while the surface is still alive) -- matches
    // every pre-promotion sample's own `ok = false; break;` contract. The
    // caller should stop calling this PresentLoop entirely and proceed to
    // an error exit.
    Failed,
    // [Issue #73] The underlying native window is confirmed gone --
    // `abandonNativeHandle()` has already been called on the `Window`
    // passed to `create()`. Matches every pre-promotion sample's own
    // `running = false` (non-fatal) contract -- the caller should stop
    // calling this PresentLoop and proceed to its ORDINARY teardown path
    // (see `shouldSkipTeardownAfterDeviceLoss()` above for the one
    // additional check that path itself needs).
    SurfaceLost,
};

// Owns every swapchain-DEPENDENT resource a present loop needs kept in sync
// with the live `VkSwapchainKHR`: per-swapchain-image `VkImageView`s,
// `rx::rhi::FrameSync` (per-frame-in-flight sync + per-image
// render-finished semaphores), and -- when constructed with a
// `RenderGraph`/`Executor` pair -- that graph's own compiled+realized
// state. `RenderGraph::compile()`/`Executor::realize()` are funneled ONLY
// through this class's own single `recreateAndDependents()` call site
// [Issue #36's own "exactly one recreation call site" design rule,
// generalized from one sample to every present-loop consumer].
//
// Not copyable or movable (owns live VkDevice-scoped Vulkan objects handed
// out via `std::optional<PresentLoop>` from `create()`, matching
// `rx::rhi::FrameSync`'s and `rx::graph::Executor`'s own established
// pattern in this codebase).
class PresentLoop {
public:
    struct CreateInfo {
        // Both required. Not owned -- must outlive this PresentLoop.
        rx::rhi::Device* device = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;

        // Required -- used ONLY for `Window::abandonNativeHandle()` on a
        // newly-detected surface loss [Issue #73]. This class never calls
        // `Window::pumpEvents()`/reads input state; event pumping stays
        // entirely the caller's own job (see this header's own top
        // comment). Not owned -- must outlive this PresentLoop.
        rx::platform::Window* window = nullptr;

        // OPTIONAL. Both null, or both non-null -- `create()` rejects
        // exactly one being set. When non-null, `create()`'s own first
        // `RenderGraph::compile()`+`Executor::realize()` call happens
        // internally (against the swapchain's shape at creation time), and
        // every later `recreateAndDependents()` call re-runs `compile()`
        // unconditionally, calling `realize()` again only when `compile()`
        // reports it actually did real work [Phase 5 Task 5 row 10 -- the
        // extent-unchanged recompile-skip now lives inside
        // `RenderGraph::compile()` itself, so this class only needs to
        // read its return value, never re-derive the decision]. When both
        // are null (samples with no RenderGraph -- hand-rolled dynamic
        // rendering directly against `FrameContext::cmd`), this class never
        // touches either. Not owned -- must outlive this PresentLoop.
        rx::graph::RenderGraph* graph = nullptr;
        rx::graph::Executor* executor = nullptr;

        // [Task 3 ambiguity resolution #1, `CompileInfo::backbufferFinalLayout`]
        // Forwarded verbatim into every `CompileInfo` this class builds --
        // irrelevant when `graph == nullptr`.
        VkImageLayout backbufferFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        // OPTIONAL -- see `OnRecreateFn`'s own comment above. Empty
        // (default) for every sample with no swapchain-extent-sized
        // resource outside a RenderGraph this loop already manages.
        OnRecreateFn onRecreate;

        // OPTIONAL [Phase 4 Task 10, spec D24] -- forwarded verbatim into
        // every `FrameSync::advanceFrame(allocatorForBudgetRefresh)` call
        // `runFrame()` makes internally, keeping `Allocator::report()`'s
        // `VK_EXT_memory_budget`-backed numbers fresh every real frame
        // exactly like a caller driving `FrameSync` directly already could
        // (see `FrameSync::advanceFrame()`'s own comment). Null (default)
        // for every sample that never calls `Allocator::report()`. Not
        // owned -- must outlive this PresentLoop.
        rx::rhi::Allocator* allocatorForBudgetRefresh = nullptr;
    };

    PresentLoop(PresentLoop&&) noexcept;
    PresentLoop& operator=(PresentLoop&&) noexcept;
    PresentLoop(const PresentLoop&) = delete;
    PresentLoop& operator=(const PresentLoop&) = delete;

    // Destructor contract mirrors `rx::rhi::FrameSync`'s own exactly (this
    // class's `FrameSync` member is one of the things it destroys): the
    // caller MUST have already driven the owning `VkDevice` to a real idle
    // point (`vkDeviceWaitIdle`) before a `PresentLoop` is destroyed.
    ~PresentLoop();

    // Builds this loop's own `FrameSync` + per-swapchain-image
    // `VkImageView`s against `info.device`'s CURRENT swapchain, and -- iff
    // `info.graph`/`info.executor` are set -- performs the first
    // `RenderGraph::compile()`+`Executor::realize()` call. Returns
    // std::nullopt (logged) on any underlying failure, or if exactly one
    // of `info.graph`/`info.executor` is set without the other.
    static std::optional<PresentLoop> create(const CreateInfo& info);

    // The full acquire -> status-handle -> frame-body -> submit -> present
    // -> advance cycle [the scope-growth comment's own named shape].
    // `frameBody` is invoked EXACTLY ONCE, and only once acquire has
    // resolved to a real image this call intends to render into and
    // present (never on a Suspended retry or an absorbed NeedsRecreate) --
    // see `Result`'s own comment for the full per-value contract.
    //
    // [Phase 4 exit-review I1] `vkWaitForFences()` for the about-to-be-used
    // frame-in-flight slot happens BEFORE `frameBody` is ever invoked --
    // any per-FIF resource write inside `frameBody` (e.g.
    // `rx::rhi::PerFrameStorageBuffer::write(ctx.frameInFlightIndex, ...)`)
    // is therefore always safe, by construction, without `frameBody`
    // needing to perform or reason about that wait itself.
    Result runFrame(const FrameBodyFn& frameBody);

    // Explicit recreation trigger for a caller-detected proactive cause
    // (present-mode toggle, F11 fullscreen toggle, live drag-resize via
    // `shouldRecreateForPixelSize()` below) -- the SAME orchestration
    // `runFrame()` itself calls internally on `NeedsRecreate`/`Suspended`,
    // exposed directly so a caller's own event-handling code can trigger it
    // proactively, ahead of the driver ever reporting `NeedsRecreate`.
    //
    // `extentOverrideForTesting`: forwarded verbatim to
    // `Device::recreateSwapchain()`'s own identically-named/identically-
    // purposed dependency-injection seam (`device.h`) -- a GPU test can
    // call `recreateAndDependents(VkExtent2D{0, 0})` directly to exercise
    // the Suspended path without a display server that will actually
    // report a zero-sized surface. Production call sites never pass this
    // (the default, std::nullopt, re-queries the real surface).
    Result recreateAndDependents(std::optional<VkExtent2D> extentOverrideForTesting = std::nullopt);

    // [Issue #36] Pure-decision wrapper around `pixelSizeRequiresRecreate()`
    // above, pre-bound to this instance's own `lastHandledExtent()` -- a
    // caller's own live-drag-resize handling becomes
    // `if (loop.shouldRecreateForPixelSize(window.lastPixelSizeEvent())) { loop.recreateAndDependents(); }`,
    // with no `lastHandledExtent` bookkeeping left for the caller to get
    // wrong.
    [[nodiscard]] bool shouldRecreateForPixelSize(VkExtent2D observed) const {
        return pixelSizeRequiresRecreate(lastHandledExtent_, observed);
    }

    [[nodiscard]] bool isSurfaceLost() const { return surfaceLost_; }
    [[nodiscard]] bool isSuspended() const { return device_->isSuspended(); }
    [[nodiscard]] VkExtent2D lastHandledExtent() const { return lastHandledExtent_; }
    [[nodiscard]] uint32_t framesInFlight() const { return frameSync_->framesInFlight(); }

private:
    PresentLoop() = default;

    bool rebuildSwapchainViews();
    void destroySwapchainViews();

    rx::rhi::Device* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    rx::platform::Window* window_ = nullptr;
    rx::graph::RenderGraph* graph_ = nullptr;
    rx::graph::Executor* executor_ = nullptr;
    VkImageLayout backbufferFinalLayout_ = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    OnRecreateFn onRecreate_;
    rx::rhi::Allocator* allocatorForBudgetRefresh_ = nullptr;

    std::optional<rx::rhi::FrameSync> frameSync_;
    std::vector<VkImageView> swapchainViews_;

    VkExtent2D lastHandledExtent_{0, 0};
    // [Issue #73] Latched true the moment `Device::isSurfaceLost()` is
    // first observed by this class -- mirrors `Device`'s own latch
    // (defense-in-depth: this class checks its OWN copy rather than
    // re-querying `device_->isSurfaceLost()` at every call, so its value
    // stays well-defined even across a hypothetical future `device_`
    // pointer swap; in practice the two always agree).
    bool surfaceLost_ = false;
};

}  // namespace rx::frame_loop
