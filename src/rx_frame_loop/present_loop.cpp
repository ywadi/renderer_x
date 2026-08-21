#include <rx_frame_loop/present_loop.h>

#include <rx_core/log.h>
#include <rx_core/profile.h>

#include <chrono>
#include <thread>
#include <utility>

namespace rx::frame_loop {

bool pixelSizeRequiresRecreate(VkExtent2D lastHandled, VkExtent2D observed) {
    if (observed.width == 0 || observed.height == 0) {
        return false;
    }
    return observed.width != lastHandled.width || observed.height != lastHandled.height;
}

bool f11TogglesFullscreen(bool imguiWantsKeyboard) { return !imguiWantsKeyboard; }

bool shouldSkipTeardownAfterDeviceLoss(VkResult waitIdleResult, bool surfaceLost) {
    return waitIdleResult == VK_ERROR_DEVICE_LOST && surfaceLost;
}

PresentLoop::PresentLoop(PresentLoop&&) noexcept = default;
PresentLoop& PresentLoop::operator=(PresentLoop&&) noexcept = default;
PresentLoop::~PresentLoop() { destroySwapchainViews(); }

void PresentLoop::destroySwapchainViews() {
    if (device_ == nullptr) {
        return;  // moved-from or default-constructed instance.
    }
    for (VkImageView view : swapchainViews_) {
        vkDestroyImageView(device_->device(), view, nullptr);
    }
    swapchainViews_.clear();
}

bool PresentLoop::rebuildSwapchainViews() {
    destroySwapchainViews();
    for (VkImage image : device_->swapchainImages()) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = device_->swapchainFormat();
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device_->device(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
            RX_LOG_ERROR("rx_frame_loop: vkCreateImageView(swapchain) failed");
            return false;
        }
        swapchainViews_.push_back(view);
    }
    return true;
}

std::optional<PresentLoop> PresentLoop::create(const CreateInfo& info) {
    if (info.device == nullptr || info.surface == VK_NULL_HANDLE || info.window == nullptr) {
        RX_LOG_ERROR("rx_frame_loop: PresentLoop::create requires a non-null device/surface/window");
        return std::nullopt;
    }
    if ((info.graph == nullptr) != (info.executor == nullptr)) {
        RX_LOG_ERROR("rx_frame_loop: PresentLoop::create requires graph and executor to be BOTH null or BOTH "
                     "non-null");
        return std::nullopt;
    }

    PresentLoop loop;
    loop.device_ = info.device;
    loop.surface_ = info.surface;
    loop.window_ = info.window;
    loop.graph_ = info.graph;
    loop.executor_ = info.executor;
    loop.backbufferFinalLayout_ = info.backbufferFinalLayout;
    loop.onRecreate_ = info.onRecreate;
    loop.allocatorForBudgetRefresh_ = info.allocatorForBudgetRefresh;

    auto frameSync = rx::rhi::FrameSync::create(loop.device_->device(), loop.device_->graphicsQueueFamily(),
                                                 static_cast<uint32_t>(loop.device_->swapchainImages().size()));
    if (!frameSync.has_value()) {
        RX_LOG_ERROR("rx_frame_loop: PresentLoop::create: FrameSync::create failed");
        return std::nullopt;
    }
    loop.frameSync_.emplace(std::move(*frameSync));

    if (!loop.rebuildSwapchainViews()) {
        return std::nullopt;
    }

    if (loop.graph_ != nullptr) {
        rx::graph::CompileInfo compileInfo;
        compileInfo.swapchainWidth = loop.device_->swapchainExtent().width;
        compileInfo.swapchainHeight = loop.device_->swapchainExtent().height;
        compileInfo.swapchainFormat = loop.device_->swapchainFormat();
        compileInfo.backbufferFinalLayout = loop.backbufferFinalLayout_;
        loop.graph_->compile(compileInfo);  // first call always performs real work -- return value unused here.
        loop.executor_->realize(*loop.graph_);
    }

    loop.lastHandledExtent_ = loop.device_->swapchainExtent();
    return loop;
}

Result PresentLoop::recreateAndDependents(std::optional<VkExtent2D> extentOverrideForTesting) {
    vkDeviceWaitIdle(device_->device());
    if (!device_->recreateSwapchain(surface_, extentOverrideForTesting)) {
        return Result::Failed;
    }
    if (device_->isSurfaceLost()) {
        RX_LOG_INFO("rx_frame_loop: the present window's native handle is gone -- stopping without touching the "
                    "surface further [Issue #73]");
        // [Issue #73] Tell Window it must not attempt SDL_DestroyWindow() on
        // this now-invalid native handle during its own eventual teardown --
        // see Window::abandonNativeHandle()'s own comment for the fatal
        // Xlib-error-handler crash this specifically avoids.
        window_->abandonNativeHandle();
        surfaceLost_ = true;
        return Result::SurfaceLost;
    }
    if (device_->isSuspended()) {
        // [D25] No live swapchain images/format/extent to rebuild views/
        // FrameSync/the render graph's transients against -- the caller's
        // own retry (runFrame()'s Suspended branch) calls this again every
        // ~16ms until a real extent returns.
        return Result::Ok;
    }

    if (!rebuildSwapchainViews()) {
        return Result::Failed;
    }
    if (!frameSync_->onSwapchainRecreated(static_cast<uint32_t>(device_->swapchainImages().size()))) {
        return Result::Failed;
    }

    // [Samples 03/04's own pattern] The caller's own per-swapchain-extent
    // resource outside any RenderGraph this loop manages (e.g. a hand-rolled
    // depth buffer) -- see OnRecreateFn's own comment for why this runs
    // here, before the RenderGraph recompile/realize step below.
    if (onRecreate_ && !onRecreate_(device_->swapchainExtent())) {
        return Result::Failed;
    }

    // [Phase 5 Task 5, row 10] RenderGraph::compile() now decides FOR
    // ITSELF whether this CompileInfo actually changed anything since the
    // last successful compile() -- this class just reads that answer
    // (compiledForReal) to decide whether the matching Executor::realize()
    // is worth paying for, instead of re-deriving the extent-comparison
    // decision itself the way the sample-local original did.
    bool compiledForReal = false;
    if (graph_ != nullptr) {
        rx::graph::CompileInfo compileInfo;
        compileInfo.swapchainWidth = device_->swapchainExtent().width;
        compileInfo.swapchainHeight = device_->swapchainExtent().height;
        compileInfo.swapchainFormat = device_->swapchainFormat();
        compileInfo.backbufferFinalLayout = backbufferFinalLayout_;
        compiledForReal = graph_->compile(compileInfo);
    }

    if (compiledForReal) {
        executor_->realize(*graph_);
    }

    lastHandledExtent_ = device_->swapchainExtent();
    return Result::Ok;
}

Result PresentLoop::runFrame(const FrameBodyFn& frameBody) {
    RX_ZONE;
    if (surfaceLost_) {
        // Defense-in-depth -- a caller is expected to stop calling
        // runFrame() once SurfaceLost is returned, matching Device's own
        // contract; this guard exists for the same "not expected to be
        // reachable in this loop's own control flow" reason the
        // sample-local original's top-level SurfaceLost branches
        // documented themselves.
        return Result::SurfaceLost;
    }

    const VkDevice vkDevice = device_->device();
    VkFence fence = frameSync_->currentFence();
    vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);

    auto acquire = device_->acquireNextImage(frameSync_->currentImageAvailableSemaphore());
    if (acquire.status == rx::rhi::SwapchainStatus::SurfaceLost) {
        window_->abandonNativeHandle();
        surfaceLost_ = true;
        return Result::SurfaceLost;
    }
    if (acquire.status == rx::rhi::SwapchainStatus::Suspended) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return recreateAndDependents() == Result::Failed ? Result::Failed : Result::Skipped;
    }
    if (acquire.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
        const Result recreateResult = recreateAndDependents();
        if (recreateResult == Result::Failed) {
            return Result::Failed;
        }
        // [Issue #73] recreateAndDependents() may have just detected
        // surface loss -- propagate it (the frame this call started is
        // abandoned; nothing was recorded/submitted for it).
        return recreateResult == Result::SurfaceLost ? Result::SurfaceLost : Result::Skipped;
    }
    if (acquire.status == rx::rhi::SwapchainStatus::DeviceLost) {
        RX_LOG_ERROR("rx_frame_loop: device lost during acquireNextImage; exiting present loop");
        return Result::Failed;
    }

    vkResetFences(vkDevice, 1, &fence);
    VkCommandBuffer cmd = frameSync_->currentCommandBuffer();
    vkResetCommandPool(vkDevice, frameSync_->currentCommandPool(), 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    FrameContext ctx;
    ctx.frameInFlightIndex = frameSync_->currentFrameIndex();
    ctx.frameNumber = frameSync_->frameNumber();
    ctx.imageIndex = acquire.imageIndex;
    ctx.image = device_->swapchainImages()[acquire.imageIndex];
    ctx.view = swapchainViews_[acquire.imageIndex];
    ctx.extent = device_->swapchainExtent();
    ctx.cmd = cmd;

    if (frameBody) {
        frameBody(ctx);
    }

    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    VkSemaphore waitSem = frameSync_->currentImageAvailableSemaphore();
    submitInfo.pWaitSemaphores = &waitSem;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkSemaphore signalSem = frameSync_->renderFinishedSemaphore(acquire.imageIndex);
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSem;
    if (vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
        return Result::Failed;
    }

    auto presentStatus = device_->present(acquire.imageIndex, signalSem);
    if (presentStatus == rx::rhi::SwapchainStatus::SurfaceLost) {
        surfaceLost_ = true;
        // Matches the sample-local original exactly: the frame just
        // submitted above DID complete this iteration's own work, so
        // advanceFrame()/RX_FRAME_MARK below still run -- only the RETURN
        // VALUE communicates the stop.
    } else if (presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate) {
        if (recreateAndDependents() == Result::Failed) {
            return Result::Failed;
        }
    } else if (presentStatus == rx::rhi::SwapchainStatus::DeviceLost) {
        RX_LOG_ERROR("rx_frame_loop: device lost during present; exiting present loop");
        return Result::Failed;
    }

    frameSync_->advanceFrame(allocatorForBudgetRefresh_);
    RX_FRAME_MARK;

    return surfaceLost_ ? Result::SurfaceLost : Result::Ok;
}

}  // namespace rx::frame_loop
