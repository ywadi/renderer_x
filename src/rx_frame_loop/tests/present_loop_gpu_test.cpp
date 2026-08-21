// present_loop_gpu_test.cpp -- [Phase 5 Task 5, ticket #41 rows 8-11] GPU
// integration tests for rx::frame_loop::PresentLoop: the orchestration
// itself (fence/acquire/status-branch/recreate/submit/present/advance)
// inherently needs a real windowed rx::rhi::Device -- this module's own
// pure decision functions are covered device-free in pure_decisions_test.cpp
// (TDD-first, per this phase's binding constraint); this file proves the
// STATEFUL orchestration built on top of them, mirroring
// src/rx_debug_ui/tests/test_overlay_gpu.cpp's own windowed-but-headless
// fixture pattern (a real VkSurfaceKHR via an invisible window, nothing
// ever actually displayed on screen) plus
// src/rx_rhi_vk/tests/window_state_test.cpp's own Suspended-call-count and
// real-resize conventions, both applied here through PresentLoop instead
// of Device directly.
#include <doctest/doctest.h>
#include <rx_frame_loop/present_loop.h>

#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_task/scheduler.h>

#include <chrono>
#include <memory>
#include <optional>
#include <thread>

using namespace rx::frame_loop;
using namespace rx::graph;

namespace {

struct GpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    VkSurfaceKHR surface;
};

std::optional<GpuFixture> makeFixture(const char* title, bool visible = false, bool resizable = false) {
    auto window = rx::platform::Window::create(title, 64, 64, visible, resizable);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_frame_loop GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_frame_loop GPU "
                 "test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    return GpuFixture{std::move(*window), std::move(*context), std::move(*device), surface};
}

// A minimal single-pass graph declaring the backbuffer as its only color
// output, with an intentionally empty setExecute() -- this file's own tests
// care about the LOOP mechanics (compile/realize/execute wiring, barrier
// correctness, zero validation errors), never pixel content, so there is
// nothing to gain by drawing anything real (that is test_execute_gpu.cpp's
// own, already-covered scope).
void declareTrivialGraph(RenderGraph& graph) {
    AttachmentDesc bbDesc;
    bbDesc.format = VK_FORMAT_R16G16B16A16_SFLOAT;  // overridden by compile() for the real backbuffer resource.
    graph.addPass("noop").addColorOutput("bb", bbDesc).setExecute([](PassContext&) {});
    graph.setBackbufferSource("bb");
}

}  // namespace

TEST_CASE("PresentLoop::create + runFrame() (no RenderGraph): N steady-state frames record/submit/present "
          "cleanly, frameBody invoked exactly once per Ok result, zero validation errors") {
    auto fixture = makeFixture("rx_frame_loop_no_graph");
    if (!fixture.has_value()) {
        return;
    }

    auto loop = PresentLoop::create(PresentLoop::CreateInfo{&fixture->device, fixture->surface, &fixture->window});
    REQUIRE(loop.has_value());
    CHECK(loop->framesInFlight() == rx::rhi::FrameSync::kFramesInFlight);

    int frameBodyCalls = 0;
    for (int i = 0; i < 6; ++i) {
        const Result result = loop->runFrame([&](const FrameContext& ctx) {
            ++frameBodyCalls;
            CHECK(ctx.cmd != VK_NULL_HANDLE);
            CHECK(ctx.image != VK_NULL_HANDLE);
            CHECK(ctx.view != VK_NULL_HANDLE);
            CHECK(ctx.frameInFlightIndex < loop->framesInFlight());
            // A hand-rolled, no-RenderGraph pass (matches samples 01-06's
            // own present-loop style): transition the swapchain image
            // straight to PRESENT_SRC_KHR with no draw work at all --
            // legal, and exercises this class's "graph == nullptr" path
            // performing zero compile()/realize() work of its own.
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.image = ctx.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);
        });
        REQUIRE(result == Result::Ok);
    }
    CHECK(frameBodyCalls == 6);

    vkDeviceWaitIdle(fixture->device.device());
    loop.reset();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("PresentLoop::create (WITH a RenderGraph) performs the first compile()+realize() internally; "
          "runFrame() drives Executor::execute() through the frame-body callback across N steady-state frames "
          "with zero validation errors") {
    auto fixture = makeFixture("rx_frame_loop_with_graph");
    if (!fixture.has_value()) {
        return;
    }
    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    RenderGraph graph;
    declareTrivialGraph(graph);

    auto loop = PresentLoop::create(PresentLoop::CreateInfo{&fixture->device, fixture->surface, &fixture->window,
                                                              &graph, executor.get()});
    REQUIRE(loop.has_value());

    for (int i = 0; i < 6; ++i) {
        const Result result = loop->runFrame([&](const FrameContext& ctx) {
            executor->execute(graph, ctx.cmd, ctx.image, ctx.view, ctx.extent);
        });
        REQUIRE(result == Result::Ok);
    }

    vkDeviceWaitIdle(fixture->device.device());
    loop.reset();
    executor.reset();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("PresentLoop::create rejects exactly one of graph/executor being set without the other") {
    auto fixture = makeFixture("rx_frame_loop_bad_createinfo");
    if (!fixture.has_value()) {
        return;
    }
    RenderGraph graph;
    declareTrivialGraph(graph);

    PresentLoop::CreateInfo info{&fixture->device, fixture->surface, &fixture->window};
    info.graph = &graph;
    info.executor = nullptr;
    CHECK_FALSE(PresentLoop::create(info).has_value());
}

TEST_CASE("PresentLoop::CreateInfo::onRecreate [samples 03/04's own hand-rolled-depth-buffer pattern]: fires "
          "exactly once per REAL recreation, receives the new extent, and a false return propagates as "
          "Result::Failed; never fires while suspended") {
    auto fixture = makeFixture("rx_frame_loop_on_recreate");
    if (!fixture.has_value()) {
        return;
    }

    int callCount = 0;
    VkExtent2D lastSeenExtent{};
    PresentLoop::CreateInfo info{&fixture->device, fixture->surface, &fixture->window};
    info.onRecreate = [&](VkExtent2D extent) {
        ++callCount;
        lastSeenExtent = extent;
        return true;
    };
    auto loop = PresentLoop::create(info);
    REQUIRE(loop.has_value());
    CHECK(callCount == 0);  // create() never invokes onRecreate -- only recreateAndDependents() does.

    // A recreation that enters Suspended must NOT invoke onRecreate (a
    // zero-extent depth-buffer create would itself be a validation error --
    // exactly the hazard the sample-local original's own comment names).
    REQUIRE(loop->recreateAndDependents(VkExtent2D{0, 0}) == Result::Ok);
    CHECK(callCount == 0);

    // Resuming (real, non-overridden query) IS a real recreation.
    REQUIRE(loop->recreateAndDependents() == Result::Ok);
    CHECK(callCount == 1);
    CHECK(lastSeenExtent.width == loop->lastHandledExtent().width);
    CHECK(lastSeenExtent.height == loop->lastHandledExtent().height);

    // A false return from onRecreate is a hard failure.
    PresentLoop::CreateInfo failingInfo{&fixture->device, fixture->surface, &fixture->window};
    failingInfo.onRecreate = [](VkExtent2D) { return false; };
    auto failingLoop = PresentLoop::create(failingInfo);
    REQUIRE(failingLoop.has_value());
    CHECK(failingLoop->recreateAndDependents() == Result::Failed);

    vkDeviceWaitIdle(fixture->device.device());
    loop.reset();
    failingLoop.reset();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("PresentLoop: the Suspended path [extentOverrideForTesting DI seam, mirroring Device::recreateSwapchain's "
          "own]: runFrame() called while suspended never invokes frameBody and issues NO real "
          "vkAcquireNextImageKHR/vkQueuePresentKHR call for that attempt, and internally resumes once a real "
          "extent is observed again") {
    auto fixture = makeFixture("rx_frame_loop_suspended");
    if (!fixture.has_value()) {
        return;
    }

    auto loop = PresentLoop::create(PresentLoop::CreateInfo{&fixture->device, fixture->surface, &fixture->window});
    REQUIRE(loop.has_value());
    CHECK_FALSE(loop->isSuspended());

    // Force the suspended-present state via the SAME dependency-injection
    // seam Device::recreateSwapchain() itself exposes for exactly this
    // purpose (device.h) -- PresentLoop::recreateAndDependents() forwards
    // it verbatim.
    REQUIRE(loop->recreateAndDependents(VkExtent2D{0, 0}) == Result::Ok);
    CHECK(loop->isSuspended());

    const uint64_t acquireBefore = fixture->device.acquireCallCount();
    const uint64_t presentBefore = fixture->device.presentCallCount();

    // This fixture's own window is real (if tiny/invisible), so the VERY
    // NEXT real (non-overridden) surface query -- issued internally by
    // runFrame()'s own Suspended-retry branch -- reports a genuine nonzero
    // extent and resumes immediately (there is no way to sustain a truly
    // zero-extent LIVE window here without an actual OS minimize event;
    // window_state_test.cpp's own N-frame flat-suspended coverage exercises
    // Device directly against a synthetic extentOverride sequence for
    // exactly this reason). What THIS one call proves instead: at the
    // moment `Device::acquireNextImage()` was actually invoked inside this
    // call, the device was STILL suspended (the override above is only
    // consumed once, by the PRIOR recreateAndDependents() call), so it took
    // the suspended short-circuit (no real vkAcquireNextImageKHR) rather
    // than the ordinary path -- `frameBody` is never invoked for a Skipped
    // result, matching Device's own acquireCallCount()/presentCallCount()
    // contract exactly.
    bool frameBodyCalled = false;
    const Result result = loop->runFrame([&](const FrameContext&) { frameBodyCalled = true; });
    CHECK(result == Result::Skipped);
    CHECK_FALSE(frameBodyCalled);
    CHECK(fixture->device.acquireCallCount() == acquireBefore);
    CHECK(fixture->device.presentCallCount() == presentBefore);
    // The internal retry (recreateAndDependents(), no override) already
    // resumed against this fixture's real, nonzero-extent surface.
    CHECK_FALSE(loop->isSuspended());

    vkDeviceWaitIdle(fixture->device.device());
    loop.reset();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("PresentLoop: a Device already surface-lost BEFORE the first runFrame() call [Phase 5 Task 5 review "
          "round, Medium finding #2; matrix row 9's own literal acceptance criterion: 'a targeted regression test "
          "constructs a Device already in the surface-lost state and asserts the helper's very first "
          "acquireNextImage() call is handled without falling through to frame recording']: the FIRST runFrame() "
          "call short-circuits to Result::SurfaceLost, frameBody is never invoked, and NO real "
          "vkAcquireNextImageKHR is attempted") {
    auto fixture = makeFixture("rx_frame_loop_pre_lost");
    if (!fixture.has_value()) {
        return;
    }

    auto loop = PresentLoop::create(PresentLoop::CreateInfo{&fixture->device, fixture->surface, &fixture->window});
    REQUIRE(loop.has_value());
    CHECK_FALSE(loop->isSurfaceLost());

    // Constructs the EXACT ordering matrix row 9 names -- a Device already
    // in the surface-lost state BEFORE this PresentLoop's own first
    // runFrame() call, i.e. PresentLoop's OWN surfaceLost_ latch
    // (present_loop.cpp) is still false (this fresh loop has never itself
    // observed a loss) while the Device it wraps already is (e.g. a host
    // sharing one Device across multiple PresentLoop lifetimes, or a
    // recreateSwapchain() call made outside this loop's own
    // recreateAndDependents()). This is exactly the case runFrame()'s own
    // top-of-function `if (surfaceLost_)` guard (present_loop.cpp) does
    // NOT catch (it only ever sees ITS OWN prior detections) -- the real
    // short-circuit this test targets is the SECOND one, inside the
    // acquire-status branch, which reads `device_->acquireNextImage()`'s
    // own live status rather than a cached flag. See device.h's own
    // forceSurfaceLostForTesting() comment for why this is the only way an
    // automated suite can construct this precise ordering (a genuinely
    // destroyed native window is MANUAL_VERIFICATION-only per
    // surface_loss_test.cpp's own header comment).
    rx::rhi::detail::forceSurfaceLostForTesting(fixture->device);
    REQUIRE(fixture->device.isSurfaceLost());

    const uint64_t acquireBefore = fixture->device.acquireCallCount();
    bool frameBodyCalled = false;
    const Result result = loop->runFrame([&](const FrameContext&) { frameBodyCalled = true; });

    CHECK(result == Result::SurfaceLost);
    CHECK_FALSE(frameBodyCalled);
    // No real vkAcquireNextImageKHR was ever attempted -- Device::
    // acquireNextImage()'s own surfaceLost_ short-circuit (device.cpp)
    // returns SwapchainStatus::SurfaceLost WITHOUT incrementing
    // acquireCallCount_ [Phase 4 Task 17, gate ruling #25's own "present
    // skip asserted by CALL COUNTS" convention], exactly like the
    // Suspended-path test above.
    CHECK(fixture->device.acquireCallCount() == acquireBefore);
    // runFrame() latches its OWN surfaceLost_ flag too, matching every
    // other SurfaceLost-detecting path in this class
    // (recreateAndDependents(), the mid-loop acquire/present branches) --
    // a SECOND runFrame() call would now take the top-of-function guard
    // this ordering originally bypassed.
    CHECK(loop->isSurfaceLost());

    vkDeviceWaitIdle(fixture->device.device());
    loop.reset();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("PresentLoop::recreateAndDependents() (WITH a RenderGraph) called twice with an unchanged surface is a "
          "correctness no-op -- rendering keeps working cleanly afterward [Phase 5 Task 5 row 10 integration: "
          "RenderGraph::compile()'s own recompile-skip is exercised through PresentLoop's real call path, not "
          "just rx_graph's own isolated unit tests]") {
    auto fixture = makeFixture("rx_frame_loop_recreate_noop");
    if (!fixture.has_value()) {
        return;
    }
    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    RenderGraph graph;
    declareTrivialGraph(graph);
    auto loop = PresentLoop::create(PresentLoop::CreateInfo{&fixture->device, fixture->surface, &fixture->window,
                                                              &graph, executor.get()});
    REQUIRE(loop.has_value());
    const VkExtent2D extentBefore = loop->lastHandledExtent();

    REQUIRE(loop->recreateAndDependents() == Result::Ok);
    REQUIRE(loop->recreateAndDependents() == Result::Ok);
    CHECK(loop->lastHandledExtent().width == extentBefore.width);
    CHECK(loop->lastHandledExtent().height == extentBefore.height);

    const Result result = loop->runFrame(
        [&](const FrameContext& ctx) { executor->execute(graph, ctx.cmd, ctx.image, ctx.view, ctx.extent); });
    REQUIRE(result == Result::Ok);

    vkDeviceWaitIdle(fixture->device.device());
    loop.reset();
    executor.reset();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("PresentLoop: a genuine SDL_SetWindowSize() resize + recreateAndDependents() (WITH a RenderGraph) "
          "succeeds, updates lastHandledExtent()/shouldRecreateForPixelSize(), and a subsequent runFrame() "
          "renders cleanly at the NEW extent with zero validation errors -- the real discriminator for whether "
          "Executor::realize() actually re-ran against the new shape (a stale-realized executor rendering "
          "against a mismatched backbuffer extent is exactly the class of bug row 10 exists to prevent)") {
    auto fixture = makeFixture("rx_frame_loop_live_resize", /*visible=*/true, /*resizable=*/true);
    if (!fixture.has_value()) {
        return;
    }
    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    RenderGraph graph;
    declareTrivialGraph(graph);
    auto loop = PresentLoop::create(PresentLoop::CreateInfo{&fixture->device, fixture->surface, &fixture->window,
                                                              &graph, executor.get()});
    REQUIRE(loop.has_value());
    const VkExtent2D before = loop->lastHandledExtent();

    if (!SDL_SetWindowSize(fixture->window.sdlWindow(), static_cast<int>(before.width) + 64,
                            static_cast<int>(before.height) + 48)) {
        MESSAGE("SDL_SetWindowSize failed on this video driver, skipping the remainder of this check");
        return;
    }
    int afterW = static_cast<int>(before.width);
    int afterH = static_cast<int>(before.height);
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        fixture->window.pumpEvents();
        SDL_GetWindowSizeInPixels(fixture->window.sdlWindow(), &afterW, &afterH);
        if (afterW != static_cast<int>(before.width) || afterH != static_cast<int>(before.height)) {
            break;
        }
    }
    const bool resizeGranted =
        (afterW != static_cast<int>(before.width) || afterH != static_cast<int>(before.height));
    if (!resizeGranted) {
        MESSAGE("this video driver/window-manager never actually resized the window on SDL_SetWindowSize() -- "
                 "skipping the extent-change assertions, but recreateAndDependents()/runFrame() are still "
                 "exercised below against whatever extent is actually live.");
    } else {
        CHECK(loop->shouldRecreateForPixelSize(VkExtent2D{static_cast<uint32_t>(afterW), static_cast<uint32_t>(afterH)}));
    }

    REQUIRE(loop->recreateAndDependents() == Result::Ok);
    if (resizeGranted) {
        CHECK(loop->lastHandledExtent().width == static_cast<uint32_t>(afterW));
        CHECK(loop->lastHandledExtent().height == static_cast<uint32_t>(afterH));
        CHECK_FALSE(loop->shouldRecreateForPixelSize(loop->lastHandledExtent()));
    }

    const Result result = loop->runFrame(
        [&](const FrameContext& ctx) { executor->execute(graph, ctx.cmd, ctx.image, ctx.view, ctx.extent); });
    REQUIRE(result == Result::Ok);

    vkDeviceWaitIdle(fixture->device.device());
    loop.reset();
    executor.reset();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
