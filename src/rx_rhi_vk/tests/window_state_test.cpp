// [Phase 4 Stage 1 Task 17, FG7, gate ruling #25] GPU-side tier of the
// two-tier test design (device-free tier lives in
// src/rx_platform/tests/window_test.cpp): the zero-extent guard's
// dependency-injection seam, the present-skip call-count assertion, the
// D25 "uploads not gated on suspension" interaction, and the borderless-
// fullscreen double-toggle -- all against a REAL rx::rhi::Device (real
// VkSwapchainKHR lifecycle, real validation layers). A genuine OS-level
// minimize (the true end-to-end path) is MANUAL_VERIFICATION-only per the
// matrix's own row 6 finding -- not reproducible headlessly on any driver
// this repo's existing fixtures use (see that row for the full rationale);
// this file proves the GUARD's own logic instead, via the DI seam.
#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace {

// Same skip-guarded windowed fixture shape as device_test.cpp/
// upload_test.cpp -- kept local (separate translation unit) per this
// project's own established convention (frame_sync_test.cpp's own comment
// on why it doesn't share device_test.cpp's anonymous-namespace fixture).
struct WindowStateTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    VkSurfaceKHR surface;
};

std::optional<WindowStateTestFixture> makeFixture(const char* title, bool visible = false) {
    auto window = rx::platform::Window::create(title, 64, 64, visible);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping window-state test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping window-state "
                 "test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    return WindowStateTestFixture{std::move(*window), std::move(*context), surface};
}

}  // namespace

// ===== Zero-extent guard: dependency-injection seam [matrix row 2/6] =====
TEST_CASE("Device::recreateSwapchain's extentOverride seam enters the suspended state on {0,0} without touching "
          "vkb::SwapchainBuilder, and resumes on the next real (nonzero) query -- zero validation errors") {
    auto fixture = makeFixture("rx_rhi_vk_window_state_zero_extent");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());
    CHECK_FALSE(device->isSuspended());

    // Given {0, 0} -- via the seam, NOT a real display reporting it (matrix
    // row 6's own testability caveat: no CI driver this repo uses can be
    // made to genuinely report a zero-sized surface) -- skip
    // vkb::SwapchainBuilder::build() entirely: distinguishable status
    // (isSuspended()==true), no swapchain object touched.
    REQUIRE(device->recreateSwapchain(fixture->surface, VkExtent2D{0, 0}));
    CHECK(device->isSuspended());
    CHECK(device->swapchain() == VK_NULL_HANDLE);
    CHECK(device->swapchainImages().empty());

    // A second suspended call (still {0, 0}) must be idempotent -- no
    // swapchain churn, still suspended, still zero validation errors.
    REQUIRE(device->recreateSwapchain(fixture->surface, VkExtent2D{0, 0}));
    CHECK(device->isSuspended());
    CHECK(device->swapchain() == VK_NULL_HANDLE);

    // Resume on restore: the next call with NO override re-queries the
    // real surface, which for this 64x64 hidden window is never zero (row
    // 8's own finding) -- the guard clears and a real swapchain is built,
    // through the exact same function, no second recreation path.
    REQUIRE(device->recreateSwapchain(fixture->surface));
    CHECK_FALSE(device->isSuspended());
    CHECK(device->swapchain() != VK_NULL_HANDLE);
    CHECK(device->swapchainImages().size() > 0);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Present-skip while suspended, asserted by CALL COUNTS [matrix row 7]
// ===========================================================================
TEST_CASE("While suspended, acquireNextImage()/present() return Suspended over N frames with the real "
          "vkAcquireNextImageKHR/vkQueuePresentKHR call counts staying exactly at 0 -- never wall-clock") {
    auto fixture = makeFixture("rx_rhi_vk_window_state_call_counts");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());

    REQUIRE(device->recreateSwapchain(fixture->surface, VkExtent2D{0, 0}));
    REQUIRE(device->isSuspended());

    const uint64_t acquireBaseline = device->acquireCallCount();
    const uint64_t presentBaseline = device->presentCallCount();

    constexpr int kSuspendedFrames = 60;
    for (int frame = 0; frame < kSuspendedFrames; ++frame) {
        auto acquire = device->acquireNextImage(VK_NULL_HANDLE);
        CHECK(acquire.status == rx::rhi::SwapchainStatus::Suspended);
        CHECK(acquire.imageIndex == 0);

        auto presentStatus = device->present(0, VK_NULL_HANDLE);
        CHECK(presentStatus == rx::rhi::SwapchainStatus::Suspended);
    }

    // The exact count of REAL vkAcquireNextImageKHR/vkQueuePresentKHR calls
    // issued across all 60 iterations is 0 -- a counter, never a
    // wall-clock/timing assertion (this project's own established
    // convention, e.g. plan Task 7's chunk-count counters).
    CHECK(device->acquireCallCount() == acquireBaseline);
    CHECK(device->presentCallCount() == presentBaseline);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== D25 interaction: uploads are NOT gated on suspension [matrix row 13]
// ===========================================================================
TEST_CASE("Uploader::flush() during a suspended-present Device neither blocks nor errors -- upload polling "
          "continues while presentation is paused") {
    auto fixture = makeFixture("rx_rhi_vk_window_state_uploads_not_gated");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());

    auto allocator = rx::rhi::Allocator::create(fixture->context, *device);
    REQUIRE(allocator.has_value());

    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    REQUIRE(uploader.has_value());

    // Enter the suspended state via the DI seam -- exactly like the zero-
    // extent test above. The Uploader owns its own command pool/queue,
    // entirely independent of the (now torn-down) swapchain.
    REQUIRE(device->recreateSwapchain(fixture->surface, VkExtent2D{0, 0}));
    REQUIRE(device->isSuspended());

    constexpr VkDeviceSize kSize = 256;
    std::array<uint8_t, kSize> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<uint8_t>(i);
    }
    auto dst = allocator->createHostVisibleBuffer(kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(dst.has_value());

    REQUIRE(uploader->uploadToBuffer(*dst, 0, pattern.data(), kSize));

    // "Neither blocks nor errors" [D25, matrix row 13's own acceptance
    // criterion]: wall-clock is a SANITY threshold here (mirroring Task
    // 11's own "timer around the call, not frame counters" convention),
    // not the primary correctness claim -- the primary claims are the
    // REQUIRE(...) success and the zero-validation-errors CHECK below. A
    // generous 2-second budget on a same-machine lavapipe/lightweight GPU
    // submission is well above any real cost and only catches a genuine
    // hang.
    const auto start = std::chrono::steady_clock::now();
    rx::rhi::UploadTicket ticket = uploader->flush();
    uploader->wait(ticket);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(2));

    // Still suspended -- flushing an unrelated upload must not have
    // resumed presentation as a side effect.
    CHECK(device->isSuspended());

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Borderless-fullscreen double-toggle [matrix rows 4/5/9] ===========
TEST_CASE("Window::setFullscreen() double-toggle routes through the SAME Device::recreateSwapchain() path a "
          "resize already uses -- zero validation errors accumulated across the whole sequence") {
    // Visible (not hidden): SDL/X11 fullscreen state is a real
    // window-manager interaction -- some WM-less/headless backends may not
    // honor it on an unmapped window at all, so this uses a real, visible
    // window (mirroring every --present sample) to give the toggle its
    // best real chance of taking effect, with an explicit, honest fallback
    // below when this environment's driver still doesn't actually resize
    // anything.
    auto fixture = makeFixture("rx_rhi_vk_window_state_fullscreen", /*visible=*/true);
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());

    bool anyRealResizeObserved = false;

    for (int cycle = 0; cycle < 2; ++cycle) {
        int beforeW = 0;
        int beforeH = 0;
        SDL_GetWindowSizeInPixels(fixture->window.sdlWindow(), &beforeW, &beforeH);

        if (!fixture->window.setFullscreen(true)) {
            MESSAGE("SDL_SetWindowFullscreen(true) failed on this video driver, skipping the remainder of this "
                     "cycle");
            continue;
        }
        CHECK(fixture->window.isFullscreen());
        // Borderless, not exclusive [gate ruling #25 row 4].
        CHECK(SDL_GetWindowFullscreenMode(fixture->window.sdlWindow()) == nullptr);

        // Exactly one recreation call site [matrix row 5]: the SAME
        // Device::recreateSwapchain(surface) a real resize's NeedsRecreate
        // handling already drives -- no bespoke
        // "recreateSwapchainForFullscreen" function exists to call
        // instead.
        REQUIRE(device->recreateSwapchain(fixture->surface));
        CHECK_FALSE(device->isSuspended());

        int fsW = 0;
        int fsH = 0;
        SDL_GetWindowSizeInPixels(fixture->window.sdlWindow(), &fsW, &fsH);
        if (fsW != beforeW || fsH != beforeH) {
            anyRealResizeObserved = true;
            CHECK(device->swapchainExtent().width == static_cast<uint32_t>(fsW));
            CHECK(device->swapchainExtent().height == static_cast<uint32_t>(fsH));
        }

        REQUIRE(fixture->window.setFullscreen(false));
        CHECK_FALSE(fixture->window.isFullscreen());

        REQUIRE(device->recreateSwapchain(fixture->surface));
        CHECK_FALSE(device->isSuspended());

        int windowedW = 0;
        int windowedH = 0;
        SDL_GetWindowSizeInPixels(fixture->window.sdlWindow(), &windowedW, &windowedH);
        if (windowedW == beforeW && windowedH == beforeH) {
            CHECK(device->swapchainExtent().width == static_cast<uint32_t>(windowedW));
            CHECK(device->swapchainExtent().height == static_cast<uint32_t>(windowedH));
        }
    }

    if (!anyRealResizeObserved) {
        MESSAGE("this video driver never actually resized the window on a fullscreen toggle (no window manager, "
                 "or a WM that ignores an unmapped/undecorated window's fullscreen hint) -- the resize-parity "
                 "assertions above were skipped, but recreateSwapchain() itself was still exercised after every "
                 "toggle with zero validation errors, which is this test's load-bearing claim either way");
    }

    // Accumulated across the WHOLE sequence (both cycles, both
    // directions), not just per-transition -- catches a leak that only
    // shows up on the second cycle.
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
