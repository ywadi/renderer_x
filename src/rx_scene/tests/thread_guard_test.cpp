#include <doctest/doctest.h>
#include <rx_core/debug_checks.h>
#include <rx_scene/draw_list.h>
#include <rx_scene/scene.h>
#include <rx_task/scheduler.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
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
    // [Phase 4 exit fix wave, M1] Every context string seen, in order --
    // NOT just the last one. DrawListBuilder::build()/buildShadow()'s own
    // guard fires FIRST, but the hook records-and-returns rather than
    // aborting, so execution then continues into every downstream guarded
    // Scene/DrawListBuilder accessor build()/buildShadow() themselves call
    // (aliveSpan()/worldBoundsSpan()/etc.) -- all firing too, since the
    // whole call is still running on the same wrong thread throughout. The
    // build()/buildShadow() guard tests below check CONTAINMENT in this
    // list, not an exact total count, for exactly that reason.
    std::vector<std::string> contexts;
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
    capture->contexts.emplace_back(capture->lastContext);
    // Returns normally rather than throwing/aborting -- safe here
    // specifically because every worker thread below is join()'d before
    // the next guarded call starts (mirrors debug_checks_test.cpp's own
    // identical precondition note).
}

bool contextsContain(const std::vector<std::string>& contexts, const std::string& needle) {
    for (const std::string& context : contexts) {
        if (context == needle) {
            return true;
        }
    }
    return false;
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

// [Phase 4 exit fix wave, M1] DrawListBuilder::build()/buildShadow() carried
// no entry-point guard of their own -- coverage was transitive only (the
// first Scene accessor either calls fires Scene's own guard), which
// disappears for any future code path that touches this builder's private
// scratch before Scene. An empty Scene/default Camera/invalid LightHandle
// is enough here: buildShadow() documents a dead/stale LightHandle as a
// defined empty-result query, never a throw, and the guard fires as the
// very first statement of both methods regardless of scene content.
namespace {
rx::asset::AABB fallbackShapedBoundsForDrawList(rx::asset::MeshHandle) { return rx::asset::AABB{}; }
std::span<const rx::asset::Submesh> emptySubmeshes(rx::asset::MeshHandle) { return {}; }
rx::scene::ResolvedMaterial fallbackMaterial(rx::asset::MaterialHandle) { return rx::scene::ResolvedMaterial{}; }
}  // namespace

TEST_CASE("DrawListBuilder::build/buildShadow trip RX_ASSERT_MAIN_THREAD when called from a worker thread") {
    rx::scene::Scene scene(&fallbackShapedBoundsForDrawList);
    rx::scene::Camera camera;
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    rx::scene::DrawListBuilder builder(&emptySubmeshes, &fallbackMaterial);
    rx::scene::ViewLists viewLists;
    rx::scene::ShadowLists shadowLists;

    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    std::thread buildThread([&] { builder.build(scene, camera, *scheduler, viewLists); });
    buildThread.join();
    std::thread buildShadowThread(
        [&] { builder.buildShadow(scene, rx::scene::LightHandle{}, camera, *scheduler, shadowLists); });
    buildShadowThread.join();

    // Containment, not an exact total -- each guarded call's own guard
    // fires FIRST (proving THIS fix wave's new entry-point guards are real),
    // then execution continues (the hook records-and-returns) into every
    // downstream guarded Scene/DrawListBuilder accessor build()/
    // buildShadow() call internally -- see ViolationCapture::contexts' own
    // comment for why the cascade is expected, not a bug in this test.
    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount >= 2);
    CHECK(contextsContain(capture.contexts, "rx::scene::DrawListBuilder::build"));
    CHECK(contextsContain(capture.contexts, "rx::scene::DrawListBuilder::buildShadow"));
}

TEST_CASE("DrawListBuilder::build/buildShadow do NOT trip the guard for calls genuinely on the main thread") {
    rx::scene::Scene scene(&fallbackShapedBoundsForDrawList);
    rx::scene::Camera camera;
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    rx::scene::DrawListBuilder builder(&emptySubmeshes, &fallbackMaterial);
    rx::scene::ViewLists viewLists;
    rx::scene::ShadowLists shadowLists;

    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    builder.build(scene, camera, *scheduler, viewLists);
    builder.buildShadow(scene, rx::scene::LightHandle{}, camera, *scheduler, shadowLists);

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 0);
}

#endif  // RX_DEBUG_CHECKS
