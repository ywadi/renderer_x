#include <doctest/doctest.h>
#include <rx_core/debug_checks.h>
#include <rx_scene/scene.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// thread_guard_test.cpp -- proves Scene's RX_ASSERT_MAIN_THREAD guards
// (D5) actually ENFORCE the main-thread-only contract scene.h documents on
// every public method, not merely document it. Device-free, no GPU fixture
// needed (unlike rx_asset/tests/thread_guard_test.cpp's GeometryPool
// case): mirrors rx_core/tests/debug_checks_test.cpp's own "a plain
// std::thread stands in for a chunk >= 1 worker" pattern, legitimate here
// for the identical reason that file documents -- this tests the
// thread-identity comparison the guard performs, not any real scheduler
// integration (Scene has no rx::task::Scheduler-driven caller in this
// phase at all).
//
// The whole file is guarded on RX_DEBUG_CHECKS, matching both precedent
// files' own identical guard (there is nothing left to compile when the
// mechanism itself is compiled away).
#ifdef RX_DEBUG_CHECKS

namespace {

rx::asset::AABB fallbackShapedBounds(rx::asset::MeshHandle) { return rx::asset::AABB{}; }

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
    // the next guarded call starts (mirrors debug_checks_test.cpp's own
    // identical precondition note).
}

struct ViolationHookGuard {
    ~ViolationHookGuard() {
        g_activeCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

}  // namespace

TEST_CASE("Scene::setTransform/transformsSpan trip RX_ASSERT_MAIN_THREAD when called from a worker thread") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle handle = scene.createRenderable(rx::scene::RenderableDesc{});

    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    std::thread setTransformThread([&] { scene.setTransform(handle, glm::mat4(1.0F)); });
    setTransformThread.join();
    std::thread spanThread([&] { static_cast<void>(scene.transformsSpan()); });
    spanThread.join();

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 2);
    CHECK(capture.lastContext == "rx::scene::Scene::transformsSpan");
}

TEST_CASE("Scene::setTransform/transformsSpan do NOT trip the guard for a call genuinely on the main thread") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle handle = scene.createRenderable(rx::scene::RenderableDesc{});

    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    scene.setTransform(handle, glm::mat4(1.0F));
    static_cast<void>(scene.transformsSpan());

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 0);
}

#endif  // RX_DEBUG_CHECKS
