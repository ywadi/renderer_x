#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_core/debug_checks.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

// thread_guard_test.cpp -- [Fix round 1, review IMPORTANT finding] proves
// GeometryPool's read accessors (stats()/blockCount()/
// bufferDeviceAddressEnabled()/vertexBufferDeviceAddress()/
// indexBufferDeviceAddress(), narrowed to main-thread-only this fix round
// -- see geometry_pool.h's own updated class comment) actually ENFORCE
// that contract via RX_ASSERT_MAIN_THREAD, not merely document it.
// Mirrors rx_core/tests/debug_checks_test.cpp's own "a plain std::thread
// stands in for a chunk >= 1 worker" pattern (legitimate here for the
// identical reason that file documents: this tests the thread-identity
// comparison the guard performs, not any real scheduler integration --
// GeometryPool has no rx::task::Scheduler-driven caller in this phase at
// all). Each worker thread below is joined before the next one starts,
// so there is never more than one thread touching this GeometryPool's
// state at a time -- exactly the precondition
// rx::core::debug::detail::ViolationHook's own contract comment requires
// of a test-installed hook that returns normally instead of aborting.
//
// The whole file (fixture included) is guarded on RX_DEBUG_CHECKS: the
// fixture exists only to feed the two hook-dependent TEST_CASEs below,
// so there is nothing left to compile when the mechanism itself is
// compiled away (RX_DEBUG_CHECKS is ON by default in both of this
// project's presets -- see rx_core/tests/debug_checks_test.cpp's own
// identical guard for why there is no meaningful OFF equivalent).
#ifdef RX_DEBUG_CHECKS

namespace {

struct ThreadGuardFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
};

std::optional<ThreadGuardFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping GeometryPool thread-guard test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping GeometryPool thread-guard test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    REQUIRE(uploader.has_value());

    return ThreadGuardFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator),
                               std::move(*uploader)};
}

}  // namespace

namespace {

struct ViolationCapture {
    std::mutex mutex;
    int callCount = 0;
    std::string lastContext;
};

std::atomic<ViolationCapture*> g_activeCapture{nullptr};

void captureViolationHook(const char* context) {
    ViolationCapture* capture = g_activeCapture.load(std::memory_order_relaxed);
    if (capture == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->callCount++;
    capture->lastContext = context != nullptr ? context : "";
    // Returns normally rather than throwing/aborting -- safe here
    // specifically because every worker thread below is join()'d before
    // the next guarded call starts, so no other thread is ever
    // concurrently touching this GeometryPool's state during the one
    // violating call (mirrors debug_checks_test.cpp's own identical
    // precondition note).
}

struct ViolationHookGuard {
    ~ViolationHookGuard() {
        g_activeCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

}  // namespace

TEST_CASE(
    "GeometryPool's read accessors (stats/blockCount/bufferDeviceAddressEnabled/*BufferDeviceAddress) trip "
    "RX_ASSERT_MAIN_THREAD when called from a worker thread [fix round 1]") {
    auto fixture = makeFixture("rx_asset_thread_guard");
    if (!fixture.has_value()) {
        return;
    }

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);

    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    // One worker thread per read accessor this fix round narrowed to
    // main-thread-only -- each joined before the next starts.
    std::thread statsThread([&] { pool->stats(); });
    statsThread.join();
    std::thread blockCountThread([&] { pool->blockCount(); });
    blockCountThread.join();
    std::thread bdaEnabledThread([&] { pool->bufferDeviceAddressEnabled(); });
    bdaEnabledThread.join();
    std::thread vertexAddrThread([&] { pool->vertexBufferDeviceAddress(0); });
    vertexAddrThread.join();
    std::thread indexAddrThread([&] { pool->indexBufferDeviceAddress(0); });
    indexAddrThread.join();

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 5);
    CHECK(capture.lastContext == "GeometryPool::indexBufferDeviceAddress");
}

TEST_CASE(
    "GeometryPool's read accessors do NOT trip the guard for a call genuinely on the main thread [fix round 1]") {
    auto fixture = makeFixture("rx_asset_thread_guard_legal");
    if (!fixture.has_value()) {
        return;
    }

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);

    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    pool->stats();
    pool->blockCount();
    pool->bufferDeviceAddressEnabled();
    pool->vertexBufferDeviceAddress(0);
    pool->indexBufferDeviceAddress(0);

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 0);
}

#endif  // RX_DEBUG_CHECKS
