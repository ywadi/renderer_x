#pragma once
#include <rx_rhi_vk/context.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace rx::rhi {

enum class SwapchainStatus {
    Ok,
    NeedsRecreate,
    DeviceLost,
    // [Phase 4 Task 17, FG7, gate ruling #25] The EXTENT-QUERY-DRIVEN
    // suspended-present state: acquireNextImage()/present() return this
    // WITHOUT ever calling vkAcquireNextImageKHR/vkQueuePresentKHR --
    // there is no live VkSwapchainKHR to acquire/present against while
    // suspended (see Device::recreateSwapchain()'s own comment on the
    // zero-extent guard). A caller sees this exactly when
    // Device::isSuspended() is true; it never overlaps with NeedsRecreate/
    // DeviceLost in the same call. The design-corrected trigger (matrix
    // conflict 1 / rulings-2026-08-18.md #25) is the QUERIED surface
    // extent being 0x0 -- not an SDL minimize event/flag, which never
    // fires on a real Wayland compositor-driven minimize (SDL issue
    // #13473) and is therefore never sufficient on its own.
    Suspended,
    // [Phase 4 Task 17 follow-up, Issue #73] The underlying NATIVE WINDOW
    // backing this surface is gone -- distinct from Suspended (a suspended
    // Device is expected to RESUME once a real extent returns, D25; a
    // surface-lost Device never will, because there is no window left to
    // ever report one again). Like Suspended, acquireNextImage()/present()
    // return this WITHOUT calling the real vkAcquireNextImageKHR/
    // vkQueuePresentKHR (see Device::isSurfaceLost()'s own comment for the
    // full rationale: no reliable advance-warning event exists for this
    // case, so recreateSwapchain() reacts to it by classifying the
    // surface-capabilities query's own VkResult, via isSurfaceLossResult()
    // below). A caller sees this exactly when Device::isSurfaceLost() is
    // true; it never overlaps with Suspended/NeedsRecreate/DeviceLost in
    // the same call. The expected caller response is NOT to retry (unlike
    // Suspended's "retry every frame until a real extent returns") -- it
    // is to stop calling acquireNextImage()/present()/recreateSwapchain()
    // against this Device+surface pair entirely and proceed straight to
    // an orderly teardown (drain in-flight GPU work, then destroy).
    SurfaceLost,
};

// [Phase 4 Task 17 follow-up, Issue #73, round-review hardening] Pure,
// device-free classifier: does `result` (a
// vkGetPhysicalDeviceSurfaceCapabilitiesKHR return value) mean the
// underlying NATIVE WINDOW backing this surface is gone, as opposed to a
// genuine, unrelated error (out-of-memory, device lost, ...) that must
// still hard-fail Device::recreateSwapchain() rather than being silently
// swallowed?
//
// True for VK_ERROR_SURFACE_LOST_KHR -- the Vulkan spec's own SOLE
// documented code for exactly this situation -- and, as a SPEC CAVEAT,
// ALSO for VK_ERROR_INITIALIZATION_FAILED: NOT a spec-documented return
// for this specific function at all, included here purely as an
// EMPIRICALLY-OBSERVED, vendor/WSI-backend-specific inference -- this
// project's own verified NVIDIA proprietary driver + Xcb WSI backend
// returns exactly this code (never VK_ERROR_SURFACE_LOST_KHR) when the
// query touches an already-destroyed X11 window: reproduced directly
// (Issue #73's own investigation) via a third-party XDestroyWindow()
// against a live --present window (the exact action `xdotool windowclose`
// performs) -- confirmed there is NO advance-warning SDL event for this
// case at all (neither SDL_EVENT_WINDOW_CLOSE_REQUESTED nor even
// SDL_EVENT_WINDOW_DESTROYED fires before the next Vulkan call against the
// surface fails), so this REACTIVE classification on the query's own
// result is the only place this can be caught. This out-of-spec half of
// the classification is UNVERIFIED on any other vendor/WSI backend --
// Device::recreateSwapchain() (device.cpp) emits a clearly-labeled,
// one-shot (per process) WARN log (naming the raw VkResult) every time
// this function's answer is reached via VK_ERROR_INITIALIZATION_FAILED
// rather than the spec-documented VK_ERROR_SURFACE_LOST_KHR, specifically
// so a future misclassification on an unverified driver surfaces in the
// first bug report instead of silently eating what might be a real,
// unrelated device error.
//
// VK_ERROR_DEVICE_LOST is EXPLICITLY, unconditionally excluded (checked
// first, before either of the two matches above) -- a lost DEVICE is a
// categorically different, always-fatal condition (the whole VkDevice
// this Device wraps is gone, not just this one surface) and must never be
// inferred as "just the window closing", regardless of how the
// true/false logic below might otherwise evolve.
// VK_ERROR_OUT_OF_HOST_MEMORY/VK_ERROR_OUT_OF_DEVICE_MEMORY (and anything
// else) are likewise deliberately NOT included here -- those stay
// genuine, unrecoverable Device::recreateSwapchain() failures.
[[nodiscard]] bool isSurfaceLossResult(VkResult result);

struct AcquireResult {
    SwapchainStatus status;
    uint32_t imageIndex;
};

// Present-mode control [Phase 4 Task 6, spec seed 1]. Only two choices are
// exposed at this layer -- an on/off vsync preference, not a raw
// VkPresentModeKHR -- because the actual mode is driver/surface-dependent
// (see Device::setPresentMode()'s own comment for the exact fallback
// ladder each of these resolves to). Callers that need the concrete mode
// actually in use (for logging or a HUD) read it back via
// Device::presentMode() instead of assuming one from this enum.
enum class PresentMode {
    VsyncOn,
    VsyncOff,
};

// Human-readable name for a VkPresentModeKHR, for logging/HUD use (e.g. a
// sample printing Device::presentMode() once at startup). Covers every
// value Device's own present-mode ladder can select (FIFO, MAILBOX,
// IMMEDIATE) plus FIFO_RELAXED for completeness, with "UNKNOWN" as the
// fallback for anything else (e.g. the shared-present extension modes this
// codebase never requests).
const char* presentModeName(VkPresentModeKHR mode);

class Device;

namespace detail {

// Test-only seam -- NOT part of the stable public contract, mirroring
// `rx::rhi::detail::debugSlotBufferData()`'s own carve-out convention
// (per_frame_storage_buffer.h) and `rx::material::detail::
// debugFrameBufferData()`'s (instance.h): the ONLY way this repo's own
// automated test suites can construct "a Device already in the
// surface-lost state" [Phase 5 Task 5, ticket #41 review round, Medium
// finding #2, matrix row 9's own literal acceptance-criterion text] is a
// GENUINE destroyed native window, which `surface_loss_test.cpp`'s own
// header comment already documents as MANUAL_VERIFICATION-only -- "no CI
// driver/display backend this repo's fixtures use can be made to
// genuinely destroy a live window out from under a running process the
// way a real desktop's window manager can". Directly setting the SAME
// `surfaceLost_` flag `recreateSwapchain()`'s own real
// `isSurfaceLossResult()`-classified failure path sets (device.cpp) lets
// a GPU test exercise every DOWNSTREAM consumer of that state
// (`acquireNextImage()`'s/`present()`'s own short-circuits, and --
// `present_loop_gpu_test.cpp`'s own use -- `PresentLoop::runFrame()`'s
// first-call short-circuit) deterministically, without needing the
// underlying VkSurfaceKHR/native window to have genuinely died (both stay
// perfectly valid Vulkan objects here -- only the LOGICAL state this
// class's own callers observe is forced).
void forceSurfaceLostForTesting(Device& device);

}  // namespace detail

// Device owns the logical VkDevice selected/built against a Context's
// vkb::Instance, its graphics and present queues, and a VkSwapchainKHR built
// against a caller-provided VkSurfaceKHR.
//
// Device::create() takes ownership of the VkSurfaceKHR passed to it,
// unconditionally: on success the returned Device owns and destroys it
// (together with the swapchain and device) on destruction or move-assign;
// on failure Device::create destroys the surface itself before returning
// std::nullopt. Either way, the caller must not destroy that surface handle
// itself once create() has been called with it.
//
// Thread-affinity (D5, Phase 4): create()/acquireNextImage()/present()/
// setPresentMode()/recreateSwapchain() are main-thread-only -- this Device
// owns the swapchain/present loop's own state, the same convention every
// existing caller of these methods (every sample's frame loop) already
// follows. Every other accessor (physicalDevice()/device()/queues/
// memoryBudgetExtensionEnabled()/calibratedTimestampsEnabled()/etc.) is a
// read-only snapshot, safe to call from any thread holding a valid
// reference -- see docs/threading.md.
class Device {
public:
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    friend void detail::forceSurfaceLostForTesting(Device& device);

    static std::optional<Device> create(Context& context, VkSurfaceKHR surface);

    // Task 3 (rx_graph's Executor): the same VkInstance handle this Device
    // was built against (Context::instance(), stashed at create() time --
    // see the private instance_ member below). Added because
    // Executor::create(Device&) needs to build its own
    // rx::rhi::Allocator::createRaw(physicalDevice, device, instance) from
    // a Device alone, with no separate Context& in its signature, and
    // every other accessor on this class already exposes exactly this
    // kind of already-stored handle (physicalDevice(), device(), ...) the
    // same trivial way.
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    uint32_t graphicsQueueFamily() const { return graphicsQueueFamily_; }
    VkQueue presentQueue() const { return presentQueue_; }

    // Optional DEDICATED transfer queue [Phase 4 Task 11, spec D25, gate
    // ruling #28 row 10]: acquired via vk-bootstrap's
    // `get_dedicated_queue(QueueType::transfer)` -- NEVER
    // `require_dedicated_transfer_queue()`, which would make PHYSICAL
    // DEVICE SELECTION itself fail outright on hardware lacking one (the
    // wrong primitive for an optional/fallback feature; verified against
    // vk-bootstrap's own `has_dedicated_transfer_queue()` definition: a
    // family that supports transfer but NOT graphics/compute, which many
    // devices -- including this project's own dev/CI hardware -- simply
    // do not expose). A missing dedicated queue is not a Device::create()
    // failure; it is a graceful, logged degrade (see create()) --
    // `hasDedicatedTransferQueue()` reports which case this Device landed
    // in; `transferQueue()`/`transferQueueFamily()` are only meaningful
    // when it returns true (VK_NULL_HANDLE/0 otherwise -- callers must
    // check the flag first, not assume a nonzero handle).
    //
    // ACQUISITION ONLY this phase -- nothing in this codebase submits to
    // transferQueue() yet (cross-queue submission and queue-family
    // ownership-transfer barriers stay streaming-phase policy, per the
    // toolchain-platform-rhi-design registry's "Upload/transfer asynchrony
    // policy" entry). The accessor existing and being correctly populated
    // (or correctly absent) is this phase's entire acceptance bar.
    bool hasDedicatedTransferQueue() const { return hasDedicatedTransferQueue_; }
    VkQueue transferQueue() const { return transferQueue_; }
    uint32_t transferQueueFamily() const { return transferQueueFamily_; }

    // True iff VK_EXT_calibrated_timestamps was present on the selected
    // physical device AND successfully enabled on the logical device this
    // Device wraps (see device.cpp's own comment on the optional, guarded
    // enable_extension_if_present() call in create()) [Phase 4 Stage 0
    // Task 3]. This class has no idea Tracy exists -- it only reports
    // whether this one, ordinarily-optional Vulkan extension is live;
    // rx_rhi_vk/tracy_gpu.h is the sole consumer that gives this fact any
    // profiling meaning (TracyVkContextCalibrated vs plain TracyVkContext).
    bool calibratedTimestampsEnabled() const { return calibratedTimestampsEnabled_; }

    // True iff `VK_EXT_memory_budget` was present on the selected physical
    // device AND successfully enabled on the logical device this Device
    // wraps [Phase 4 Task 10, spec D24(b), gate ruling #27] -- same
    // opportunistic, guarded `enable_extension_if_present()` pattern as
    // calibratedTimestampsEnabled() just above (see device.cpp's own
    // comment on that call). rx::rhi::Allocator::create(Context&, Device&)
    // (buffer.h) reads this to decide whether to request
    // VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT -- the two must stay in
    // LOCKSTEP. This is enforced entirely by THIS being the single source
    // of truth Allocator::create() always reads directly (buffer.h has the
    // full correction on why VMA itself does not runtime-enforce this),
    // rather than Allocator guessing or re-querying the extension itself.
    bool memoryBudgetExtensionEnabled() const { return memoryBudgetExtensionEnabled_; }

    // [Phase 4 Stage 1 Task 12, spec D26.4] True iff `bufferDeviceAddress`
    // (VkPhysicalDeviceVulkan12Features -- promoted VK_KHR_buffer_device_address
    // core in Vulkan 1.2, not an extension string) was present on the
    // selected physical device AND opportunistically enabled on the
    // logical device this Device wraps. NEVER a device-selection
    // requirement (D26.4) -- device.cpp resolves this via vk-bootstrap's
    // `enable_extension_features_if_present()` AFTER physical-device
    // selection has already succeeded, so a device lacking it still
    // builds a fully working Device; this simply reports false and one
    // log line records the degrade. rx::rhi::Allocator::create(Context&,
    // Device&) (buffer.h) reads this to decide whether to request
    // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT -- the two must stay
    // in LOCKSTEP, exactly like memoryBudgetExtensionEnabled() above (this
    // is the single source of truth Allocator::create() always reads
    // directly). rx::asset::GeometryPool (src/rx_asset) is this phase's
    // consumer: it reads this once at create() to decide whether its
    // chunk buffers carry VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    // Enablement only -- nothing in Phase 4 calls vkGetBufferDeviceAddress
    // for any purpose beyond GeometryPool's own test-observable accessor.
    bool supportsBufferDeviceAddress() const { return bufferDeviceAddressEnabled_; }

    // [Phase 4 Stage 1 Task 14, spec D10/G6] True iff `samplerAnisotropy`
    // (VkPhysicalDeviceFeatures, Vulkan 1.0 core -- NOT a features12/13
    // bit) was ENABLED on this Device's underlying VkDevice, resolved via
    // vk-bootstrap's `enable_features_if_present()` AFTER physical-device
    // selection, mirroring supportsBufferDeviceAddress()'s own opportunistic
    // pattern immediately above: a device lacking the feature still builds
    // a fully working Device; this simply reports false. rx::asset::
    // TextureCache::getOrCreateSampler() (src/rx_asset) is this phase's
    // consumer -- checking ONLY VkPhysicalDeviceFeatures::samplerAnisotropy
    // (what the hardware/driver merely ADVERTISES) without also checking
    // this accessor (what THIS device's VkDevice actually had ENABLED at
    // creation time) is a real, empirically-hit validation error
    // (VUID-VkSamplerCreateInfo-anisotropyEnable-01070), not a hypothetical
    // one -- reproduced directly by this task's own first-draft
    // texture_cache_test.cpp run before this accessor existed.
    bool supportsSamplerAnisotropy() const { return samplerAnisotropyEnabled_; }

    // [Phase 4 Stage 2 Task 22, spec D21/gate ruling #23] True iff
    // `depthClamp` (VkPhysicalDeviceFeatures, Vulkan 1.0 core) was
    // ENABLED on this Device's underlying VkDevice -- same opportunistic,
    // never-a-device-selection-requirement pattern as
    // supportsSamplerAnisotropy() immediately above. The scene-path
    // shadow-caster pipeline (rx::shadow::ShadowCasterPipeline) sets
    // `depthClampEnable=VK_TRUE` (so a caster whose geometry crosses the
    // light's near plane still casts its full silhouette instead of being
    // clipped away -- the matrix's own acceptance criterion) ONLY when
    // this accessor is true; a device lacking the feature gets
    // `depthClampEnable=VK_FALSE` instead (a caster crossing the near
    // plane can vanish/truncate on that device -- a real, documented
    // Phase-4 limitation of running on such hardware, not a crash or a
    // validation error: VUID-VkPipelineRasterizationStateCreateInfo-
    // depthClampEnable-00782 requires the feature for
    // depthClampEnable=VK_TRUE, so checking this accessor before setting
    // it is load-bearing, exactly like samplerAnisotropy's own cited VUID
    // immediately above).
    bool supportsDepthClamp() const { return depthClampEnabled_; }

    VkSwapchainKHR swapchain() const { return swapchain_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchainImages_; }
    VkFormat swapchainFormat() const { return swapchainFormat_; }
    VkExtent2D swapchainExtent() const { return swapchainExtent_; }

    // Records the present mode this Device should build its NEXT swapchain
    // against. Does NOT itself touch the live swapchain or recreate
    // anything -- recreateSwapchain() (the same path every caller already
    // drives on SwapchainStatus::NeedsRecreate, e.g. a window resize) is
    // the sole place a present-mode change actually takes effect, per this
    // task's explicit constraint against inventing a second recreation
    // flow. A caller that wants the change applied immediately (e.g. a
    // sample honoring a --vsync flag at startup, before its first frame)
    // must call recreateSwapchain(surface) itself right after this call.
    //
    // The mode actually built is resolved from `mode` against what this
    // surface/device supports, via the ladder in device.cpp's
    // selectPresentMode(): VsyncOn always resolves to FIFO (the one
    // present mode the Vulkan spec guarantees for every surface).
    // VsyncOff prefers MAILBOX (uncapped, no tearing), then IMMEDIATE
    // (uncapped, tearing possible), and only falls back to FIFO -- logging
    // a one-line warning when it does, since that silently keeps vsync
    // effectively on despite the caller asking for it off -- if this
    // surface supports neither.
    void setPresentMode(PresentMode mode);

    // The actual VkPresentModeKHR currently in use, as reported back by
    // vk-bootstrap after the ladder above resolved the most recent
    // setPresentMode() request (or, before any setPresentMode() call, the
    // explicit FIFO default create() builds every Device with -- see that
    // function's own comment). For logging/HUD use; presentModeName()
    // above turns this into a readable string.
    VkPresentModeKHR presentMode() const { return actualPresentMode_; }

    // Acquires the next available swapchain image, signaling `signal` once
    // it is safe to render into it. VK_SUBOPTIMAL_KHR is treated as success
    // here: the image is still valid to render into and present, so
    // recreation is deferred to present()'s return value instead.
    //
    // [Phase 4 Task 17, FG7] While isSuspended() is true, this returns
    // AcquireResult{SwapchainStatus::Suspended, 0} WITHOUT calling
    // vkAcquireNextImageKHR at all (skip-acquire-entirely, the
    // Khronos-tutorial pattern -- there is no live swapchain to acquire
    // against). acquireCallCount() only increments on the branch that
    // actually issues the real Vulkan call, so a caller/test can assert
    // "zero acquires while suspended" by call count, not by inference.
    //
    // [Phase 4 Task 17 follow-up, Issue #73] Same short-circuit while
    // isSurfaceLost() is true instead, returning
    // AcquireResult{SwapchainStatus::SurfaceLost, 0} -- also without
    // calling vkAcquireNextImageKHR (also excluded from
    // acquireCallCount()). Checked before isSuspended() (the two never
    // overlap in practice -- see isSurfaceLost()'s own comment).
    AcquireResult acquireNextImage(VkSemaphore signal);

    // Presents `imageIndex`, waiting on `wait` first unless it is
    // VK_NULL_HANDLE.
    //
    // [Phase 4 Task 17, FG7] Same suspended short-circuit as
    // acquireNextImage() above: while isSuspended() is true, returns
    // SwapchainStatus::Suspended without calling vkQueuePresentKHR
    // (presentCallCount() likewise only counts the real call).
    //
    // [Phase 4 Task 17 follow-up, Issue #73] Same surface-lost
    // short-circuit as acquireNextImage() above: while isSurfaceLost() is
    // true, returns SwapchainStatus::SurfaceLost without calling
    // vkQueuePresentKHR.
    SwapchainStatus present(uint32_t imageIndex, VkSemaphore wait);

    // Waits for the device to go idle, destroys the current swapchain, and
    // rebuilds it against `surface` (refreshing images/format/extent).
    // Does not take ownership of `surface` and does not touch the surface
    // this Device already owns; in the expected usage the same surface
    // handle is passed again on every call.
    //
    // Also the sole place the present mode most recently recorded via
    // setPresentMode() is actually applied [Phase 4 Task 6] -- every
    // caller of this function (resize/NeedsRecreate handling, or a sample
    // applying --vsync at startup) gets that behavior for free, with no
    // separate "apply present mode" path to call.
    //
    // ZERO-EXTENT/MINIMIZE GUARD [Phase 4 Task 17, FG7, gate ruling #25]:
    // after destroying the existing swapchain (as above), this function
    // queries the surface's CURRENT extent -- via `extentOverride` if the
    // caller supplied one, otherwise via a real
    // vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface,
    // ...) call against `surface` itself. If that extent has width==0 ||
    // height==0, this function skips vkb::SwapchainBuilder::build() ENTIRELY
    // (no swapchain object created or touched), enters the suspended-present
    // state (isSuspended() becomes true -- see acquireNextImage()/present()
    // above), and returns true (this IS the successful outcome: "no work to
    // do yet" is not a failure). Once a later call observes a nonzero
    // extent, the normal build path below runs and isSuspended() clears --
    // "resume on restore".
    //
    // `extentOverride` is a dependency-injection seam [gate ruling #25,
    // matrix row 6] added specifically so this guard's LOGIC is unit-testable
    // without a display server that will actually report a zero-sized
    // surface: a GPU test can call recreateSwapchain(surface,
    // VkExtent2D{0, 0}) directly. Production call sites never pass this
    // (std::nullopt, the default) -- every real call (resize-driven
    // NeedsRecreate, a --vsync/--fullscreen startup application, or the
    // present loop's own suspended-retry call) always re-queries the real
    // surface, which is what lets a genuine OS-level minimize on X11/Win32
    // (VkSurfaceCapabilitiesKHR::currentExtent literally becomes (0,0) --
    // Khronos WSI spec, Win32/Xcb/Xlib surfaces) get caught here BEFORE ever
    // reaching the VUID-VkSwapchainCreateInfoKHR-imageExtent-01689
    // violation this guard exists to prevent -- with NO special-casing
    // needed at any call site.
    //
    // WAYLAND NOTE (design correction, matrix conflict 1): on Wayland,
    // `currentExtent` is the (0xFFFFFFFF, 0xFFFFFFFF) "surface determines
    // its own size" sentinel, never a real (0,0) -- this guard's
    // width==0||height==0 check correctly never fires for that sentinel
    // (0xFFFFFFFF != 0), so a Wayland surface always falls through to the
    // normal build path exactly as it did before this task. This engine has
    // NO reliable zero-extent signal on Wayland at all (matrix row 3) --
    // see Window::logWaylandMinimizeLimitationOnce() for the one-shot,
    // diagnosable-not-silent acknowledgment of that gap.
    //
    // SURFACE-LOST GUARD [Phase 4 Task 17 follow-up, Issue #73]: if the
    // capabilities query above FAILS (rather than succeeding with a
    // queryable extent) with a VkResult isSurfaceLossResult() (device.h)
    // classifies as "the native window is gone", this function -- like the
    // zero-extent guard -- never touches vkb::SwapchainBuilder::build(),
    // enters the surface-lost terminal state (isSurfaceLost() becomes true
    // -- see acquireNextImage()/present() above), and returns true (also a
    // successful outcome: there is nothing left this function could do).
    // UNLIKE the zero-extent guard, this state is not expected to clear on
    // a later call against the SAME dead surface -- see isSurfaceLost()'s
    // own comment for the expected caller response (stop calling this
    // Device+surface pair entirely, proceed to teardown). Any OTHER
    // capabilities-query failure (out-of-memory, ...) is unchanged: still a
    // hard `return false`.
    bool recreateSwapchain(VkSurfaceKHR surface, std::optional<VkExtent2D> extentOverride = std::nullopt);

    // True while this Device is in the suspended-present state (the most
    // recent recreateSwapchain() call observed a 0x0 extent and skipped
    // building a real swapchain). swapchain()/swapchainImages() are
    // VK_NULL_HANDLE/empty for the whole duration. Cleared the next time
    // recreateSwapchain() observes a nonzero extent.
    bool isSuspended() const { return suspended_; }

    // [Phase 4 Task 17 follow-up, Issue #73] True while this Device is in
    // the surface-lost terminal state: the most recent recreateSwapchain()
    // call's own vkGetPhysicalDeviceSurfaceCapabilitiesKHR query failed
    // with a VkResult isSurfaceLossResult() (device.h, above) classifies as
    // "the native window is gone", rather than succeeding with a
    // queryable (possibly zero) extent. swapchain()/swapchainImages() are
    // VK_NULL_HANDLE/empty for the whole duration, exactly like
    // isSuspended() -- but UNLIKE isSuspended(), this is never expected to
    // clear on its own: there is no window left to ever report a real
    // extent again. Only clears if a caller supplies a genuinely different
    // (live) VkSurfaceKHR to a later recreateSwapchain() call that
    // succeeds. Callers should treat this exactly like a graceful request
    // to stop: cease calling acquireNextImage()/present()/
    // recreateSwapchain() against this Device+surface pair and proceed to
    // an orderly teardown -- see this project's samples for the
    // established shape (drain in-flight GPU work via vkDeviceWaitIdle(),
    // then destroy).
    bool isSurfaceLost() const { return surfaceLost_; }

    // Test/diagnostic accessors [Phase 4 Task 17, gate ruling #25: "present
    // skip asserted by CALL COUNTS, never wall-clock"] -- mirror
    // rx::rhi::Uploader's everUsedDirectPath()/ringWrapCount() convention
    // (upload.h): production code has no need for these. Count the number
    // of times this Device has actually issued the real
    // vkAcquireNextImageKHR/vkQueuePresentKHR call respectively, across its
    // whole lifetime -- NOT the number of times acquireNextImage()/
    // present() were merely CALLED (a call made while suspended returns
    // early and does not increment either counter). A test drives N
    // suspended frames and asserts these stay exactly at their pre-loop
    // values.
    uint64_t acquireCallCount() const { return acquireCallCount_; }
    uint64_t presentCallCount() const { return presentCallCount_; }

private:
    Device() = default;

    void destroyAll();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = 0;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue transferQueue_ = VK_NULL_HANDLE;
    uint32_t transferQueueFamily_ = 0;
    bool hasDedicatedTransferQueue_ = false;
    bool calibratedTimestampsEnabled_ = false;
    bool memoryBudgetExtensionEnabled_ = false;
    bool bufferDeviceAddressEnabled_ = false;
    bool samplerAnisotropyEnabled_ = false;
    bool depthClampEnabled_ = false;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{0, 0};

    // Desired mode recorded by setPresentMode(), consulted by
    // recreateSwapchain() (and create()'s own equivalent inline logic) the
    // next time either builds a swapchain. Defaults to VsyncOn to match
    // create()'s explicit FIFO default -- see that function's comment.
    PresentMode desiredPresentMode_ = PresentMode::VsyncOn;
    // What the ladder actually resolved `desiredPresentMode_` to, as
    // reported back by vk-bootstrap. Returned by presentMode().
    VkPresentModeKHR actualPresentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    // [Phase 4 Task 17, FG7] See recreateSwapchain()/isSuspended()'s own
    // comments above.
    bool suspended_ = false;
    // [Phase 4 Task 17 follow-up, Issue #73] See recreateSwapchain()/
    // isSurfaceLost()'s own comments above.
    bool surfaceLost_ = false;
    uint64_t acquireCallCount_ = 0;
    uint64_t presentCallCount_ = 0;
};

}  // namespace rx::rhi
