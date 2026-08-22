#include <doctest/doctest.h>
#include <rx_scene/scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <vector>

// scene_test.cpp -- device-free coverage for rx::scene::Scene [spec D19,
// Phase 4 Stage 2 Task 18; gate matrix-issue05-scene-proxies.md as
// amended by gate/rulings-2026-08-18.md's #5 section + RC5]. Every case
// here uses a trivial, in-test MeshBoundsFn (never a real
// asset::Registry) -- see scene.h's own "MESH BOUNDS RESOLUTION" comment
// for why: Registry's only way to hold NON-fallback MeshAsset content is
// GPU-backed importGltf(), which this device-free binary must not touch.

namespace {

rx::asset::AABB unitCubeBounds() {
    rx::asset::AABB box;
    box.expand(glm::vec3(-1.0F, -1.0F, -1.0F));
    box.expand(glm::vec3(1.0F, 1.0F, 1.0F));
    return box;
}

// A provider that always resolves to an invalid (never-expanded) AABB --
// the exact shape rx_asset's own makeFallbackMeshAsset() produces for a
// dead/evicted MeshHandle (see rx_asset/fallbacks.h's own comment: "zero
// submeshes -> invalid bounds").
rx::asset::AABB fallbackShapedBounds(rx::asset::MeshHandle) { return rx::asset::AABB{}; }

}  // namespace

TEST_CASE(
    "Scene renderable handle lifecycle: creation, generational reuse, and loud failure for a stale handle "
    "[gate 'Handle lifecycle' row]") {
    rx::scene::Scene scene(&fallbackShapedBounds);

    rx::scene::RenderableDesc desc;
    rx::scene::RenderableHandle h0 = scene.createRenderable(desc);
    rx::scene::RenderableHandle h1 = scene.createRenderable(desc);
    rx::scene::RenderableHandle h2 = scene.createRenderable(desc);
    CHECK(h0.generation() == 1);
    CHECK(h1.generation() == 1);
    CHECK(h2.generation() == 1);
    CHECK(scene.renderableCount() == 3);

    // Destroy the middle slot and re-create: LIFO freelist reuse with a
    // bumped generation -- mirrors rx::core::HandlePool's own convention
    // (see rx_rhi_vk/tests/bindless_test.cpp's identical
    // "register/release/re-register... reuse indices with bumped
    // generations" precedent, per the gate ruling's own instruction to
    // match that test shape).
    scene.destroyRenderable(h1);
    CHECK(scene.renderableCount() == 2);
    CHECK_FALSE(scene.isRenderableAlive(h1));

    rx::scene::RenderableHandle h1Again = scene.createRenderable(desc);
    CHECK(h1Again.index() == h1.index());
    CHECK(h1Again.generation() == 2);
    CHECK(scene.renderableCount() == 3);

    // The STALE handle (same index, superseded generation) fails EVERY
    // accessor loudly -- never silently resolves to the new occupant.
    CHECK_THROWS_AS(static_cast<void>(scene.transform(h1)), std::out_of_range);
    CHECK_THROWS_AS(scene.setTransform(h1, glm::mat4(1.0F)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(scene.layers(h1)), std::out_of_range);
    CHECK_THROWS_AS(scene.setLayers(h1, 1U), std::out_of_range);
    CHECK_FALSE(scene.isRenderableAlive(h1));

    // The NEW handle at the same index behaves normally.
    CHECK(scene.isRenderableAlive(h1Again));
    CHECK_NOTHROW(scene.setTransform(h1Again, glm::mat4(1.0F)));

    // A fresh registration after the reuse lands on a brand-new index.
    rx::scene::RenderableHandle h3 = scene.createRenderable(desc);
    CHECK(h3.index() != h0.index());
    CHECK(h3.index() != h1.index());
    CHECK(h3.index() != h2.index());
    CHECK(h3.generation() == 1);

    // Double-destroy and an entirely unknown handle are safe, idempotent
    // no-ops for destroy() specifically (mirrors
    // rx::core::HandlePool::release()'s own forgiving convention) --
    // every OTHER accessor/mutator still throws for the same unknown
    // handle.
    CHECK_NOTHROW(scene.destroyRenderable(h1));
    rx::scene::RenderableHandle bogus(999, 1);
    CHECK_NOTHROW(scene.destroyRenderable(bogus));
    CHECK_THROWS_AS(static_cast<void>(scene.transform(bogus)), std::out_of_range);
}

TEST_CASE("Scene renderable SoA columns are real, contiguous, index-parallel std::span<const T> per field [D19]") {
    rx::scene::Scene scene(&fallbackShapedBounds);

    for (uint32_t i = 0; i < 8; ++i) {
        rx::scene::RenderableDesc desc;
        desc.transform = glm::translate(glm::mat4(1.0F), glm::vec3(static_cast<float>(i), 0.0F, 0.0F));
        desc.layers = 1U << (i % 32);
        desc.channels = static_cast<uint8_t>(i);
        desc.priority = static_cast<uint8_t>(i % 8);
        desc.castsShadows = (i % 2) == 0;
        static_cast<void>(scene.createRenderable(desc));
    }

    REQUIRE(scene.transformsSpan().size() == 8);
    REQUIRE(scene.layersSpan().size() == 8);
    REQUIRE(scene.channelsSpan().size() == 8);
    REQUIRE(scene.prioritySpan().size() == 8);
    REQUIRE(scene.castsShadowsSpan().size() == 8);
    REQUIRE(scene.worldBoundsSpan().size() == 8);
    REQUIRE(scene.meshSpan().size() == 8);

    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(scene.transformsSpan()[i][3][0] == doctest::Approx(static_cast<float>(i)));
        CHECK(scene.layersSpan()[i] == (1U << (i % 32)));
        CHECK(scene.channelsSpan()[i] == i);
        CHECK(scene.prioritySpan()[i] == i % 8);
        CHECK((scene.castsShadowsSpan()[i] != 0) == ((i % 2) == 0));
    }

    // Direct proof this is real per-field SoA (one std::vector<T> per
    // column), not one std::vector<RenderableDesc>-shaped AoS row per
    // slot: the byte stride between two consecutive elements of a span
    // equals sizeof(T) exactly, never sizeof(some larger aggregate row).
    CHECK(reinterpret_cast<const std::byte*>(&scene.transformsSpan()[1]) -
              reinterpret_cast<const std::byte*>(&scene.transformsSpan()[0]) ==
          static_cast<std::ptrdiff_t>(sizeof(glm::mat4)));
    CHECK(reinterpret_cast<const std::byte*>(&scene.layersSpan()[1]) -
              reinterpret_cast<const std::byte*>(&scene.layersSpan()[0]) ==
          static_cast<std::ptrdiff_t>(sizeof(uint32_t)));
}

TEST_CASE(
    "Transform SoA column accepts a same-shaped 'previous transforms' copy with zero reshaping "
    "[transform-pool prev-frame-slot design note, issue #5 2026-08-10]") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    for (int i = 0; i < 5; ++i) {
        rx::scene::RenderableDesc desc;
        desc.transform = glm::translate(glm::mat4(1.0F), glm::vec3(static_cast<float>(i), 0.0F, 0.0F));
        static_cast<void>(scene.createRenderable(desc));
    }

    // A stub "previous transforms" array, built ENTIRELY in this test --
    // no new production API added for it. Proves transformsSpan()'s
    // existing contiguous layout is already sufficient for a future
    // double-buffered/copyable-row addition to bolt onto, with zero
    // reshaping of the live column or its accessor signature.
    std::vector<glm::mat4> previousTransforms(scene.transformsSpan().begin(), scene.transformsSpan().end());
    REQUIRE(previousTransforms.size() == scene.transformsSpan().size());
    for (size_t i = 0; i < previousTransforms.size(); ++i) {
        CHECK(previousTransforms[i] == scene.transformsSpan()[i]);
    }
}

TEST_CASE(
    "Scene::createRenderable populates world-space AABB from the mesh's stored bounds transformed by the initial "
    "transform [gate 'Per-instance bounding-box override' row]") {
    auto bounds = [](rx::asset::MeshHandle) { return unitCubeBounds(); };
    rx::scene::Scene scene(bounds);

    rx::scene::RenderableDesc desc;
    desc.transform = glm::translate(glm::mat4(1.0F), glm::vec3(5.0F, 0.0F, 0.0F));
    rx::scene::RenderableHandle h = scene.createRenderable(desc);

    const rx::asset::AABB& wb = scene.worldBounds(h);
    REQUIRE(wb.isValid());
    CHECK(wb.min.x == doctest::Approx(4.0F));
    CHECK(wb.max.x == doctest::Approx(6.0F));
    CHECK(wb.min.y == doctest::Approx(-1.0F));
    CHECK(wb.max.y == doctest::Approx(1.0F));
    CHECK(wb.min.z == doctest::Approx(-1.0F));
    CHECK(wb.max.z == doctest::Approx(1.0F));
}

TEST_CASE(
    "Scene::setTransform updates worldBounds (not just the matrix) before the next build() call [D15, gate row]") {
    auto bounds = [](rx::asset::MeshHandle) { return unitCubeBounds(); };
    rx::scene::Scene scene(bounds);

    rx::scene::RenderableDesc desc;
    rx::scene::RenderableHandle h = scene.createRenderable(desc);
    CHECK(scene.worldBounds(h).min.x == doctest::Approx(-1.0F));

    scene.setTransform(h, glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 20.0F)));

    const rx::asset::AABB& wb = scene.worldBounds(h);
    CHECK(wb.min.z == doctest::Approx(19.0F));
    CHECK(wb.max.z == doctest::Approx(21.0F));
    CHECK(wb.min.x == doctest::Approx(-1.0F));
    CHECK(scene.transform(h)[3][2] == doctest::Approx(20.0F));
}

TEST_CASE(
    "Scene re-resolves a renderable's mesh bounds through the SAME provider on every setTransform() call -- D24 "
    "at the proxy level [brief's own gate-hardening block]") {
    rx::asset::AABB currentBounds = unitCubeBounds();
    // Captured by reference: stands in for asset::Registry::mesh()'s own
    // D24 residency-tolerant behavior swapping to the fallback mesh's
    // bounds on eviction -- meshBoundsFromRegistry()'s production adapter
    // would observe the identical swap transparently, since it forwards
    // straight through Registry::mesh() with no caching of its own (see
    // scene.h's own "MESH BOUNDS RESOLUTION"/"D24 AT THE PROXY LEVEL"
    // comments).
    auto bounds = [&currentBounds](rx::asset::MeshHandle) { return currentBounds; };
    rx::scene::Scene scene(bounds);

    rx::scene::RenderableDesc desc;
    rx::scene::RenderableHandle h = scene.createRenderable(desc);
    REQUIRE(scene.worldBounds(h).isValid());
    CHECK(scene.worldBounds(h).min.x == doctest::Approx(-1.0F));

    // Simulate eviction: the provider now resolves to the fallback mesh's
    // own [zero-submesh, invalid] bounds shape.
    currentBounds = rx::asset::AABB{};

    // No dedicated "refresh" API exists on Scene -- setTransform() is
    // ALREADY the re-resolution point (its own O(1) SoA write always
    // recomputes worldBounds from a fresh provider call); a no-op
    // identity transform still exercises it.
    scene.setTransform(h, glm::mat4(1.0F));

    CHECK_FALSE(scene.worldBounds(h).isValid());  // reflects the fallback -- never a crash, never a stale value
}

TEST_CASE("RenderableDesc::priority is clamped to [0,7], default 4 [Filament precedent, gate 'Priority' row]") {
    rx::scene::RenderableDesc defaultDesc;
    CHECK(defaultDesc.priority == 4);

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle h0 = scene.createRenderable(defaultDesc);
    CHECK(scene.priority(h0) == 4);

    rx::scene::RenderableDesc tooHigh;
    tooHigh.priority = 250;
    rx::scene::RenderableHandle h1 = scene.createRenderable(tooHigh);
    CHECK(scene.priority(h1) == 7);

    rx::scene::RenderableDesc atMax;
    atMax.priority = 7;
    rx::scene::RenderableHandle h2 = scene.createRenderable(atMax);
    CHECK(scene.priority(h2) == 7);

    // [Review round 1, Minor] A representative IN-RANGE, non-default,
    // non-boundary value must pass through verbatim -- the two cases
    // above only prove clamping at/above the ceiling; this proves
    // createRenderable() does not also clamp/rewrite a value that never
    // needed clamping in the first place.
    rx::scene::RenderableDesc inRange;
    inRange.priority = 2;
    rx::scene::RenderableHandle h3 = scene.createRenderable(inRange);
    CHECK(scene.priority(h3) == 2);
}

TEST_CASE("RenderableDesc::castsShadows defaults true [RC5] and round-trips per-object") {
    rx::scene::RenderableDesc desc;
    CHECK(desc.castsShadows);

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle h = scene.createRenderable(desc);
    CHECK(scene.castsShadows(h));

    rx::scene::RenderableDesc noShadow;
    noShadow.castsShadows = false;
    rx::scene::RenderableHandle h2 = scene.createRenderable(noShadow);
    CHECK_FALSE(scene.castsShadows(h2));
}

TEST_CASE("Reserved skinning/morph attach points round-trip exactly with no truncation [preserve-later]") {
    rx::scene::RenderableDesc desc;
    CHECK_FALSE(desc.skinningBufferIndex.has_value());
    CHECK_FALSE(desc.morphTargetBufferIndex.has_value());

    desc.skinningBufferIndex = 424'242U;
    desc.morphTargetBufferIndex = 7U;

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle h = scene.createRenderable(desc);

    REQUIRE(scene.skinningBufferIndex(h).has_value());
    CHECK(scene.skinningBufferIndex(h).value() == 424'242U);
    REQUIRE(scene.morphTargetBufferIndex(h).has_value());
    CHECK(scene.morphTargetBufferIndex(h).value() == 7U);

    // A renderable that never sets them stays inert (nullopt) -- never a
    // stale value leaking in from a reused slot.
    rx::scene::RenderableHandle h2 = scene.createRenderable(rx::scene::RenderableDesc{});
    CHECK_FALSE(scene.skinningBufferIndex(h2).has_value());
    CHECK_FALSE(scene.morphTargetBufferIndex(h2).has_value());
}

TEST_CASE("Per-submesh material override is a concrete typed field [gate row, not a doc comment]") {
    using rx::asset::MaterialHandle;
    const MaterialHandle mat42(42, 1);
    const std::vector<std::optional<MaterialHandle>> overrides = {std::nullopt, mat42, std::nullopt};

    rx::scene::RenderableDesc desc;
    desc.submeshMaterialOverrides = overrides;

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle h = scene.createRenderable(desc);

    const auto stored = scene.submeshMaterialOverrides(h);
    REQUIRE(stored.size() == 3);
    CHECK_FALSE(stored[0].has_value());
    REQUIRE(stored[1].has_value());
    CHECK(stored[1].value() == mat42);
    CHECK_FALSE(stored[2].has_value());
}

TEST_CASE("layers/channels get/set API parity [matrix-issue07 ruling]") {
    rx::scene::RenderableDesc desc;
    CHECK(desc.layers == ~0U);
    CHECK(desc.channels == 0xFFU);

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle h = scene.createRenderable(desc);
    CHECK(scene.layers(h) == ~0U);
    CHECK(scene.channels(h) == 0xFFU);

    scene.setLayers(h, 0x1U);
    CHECK(scene.layers(h) == 0x1U);
    scene.setChannels(h, 0x2U);
    CHECK(scene.channels(h) == 0x2U);
}

TEST_CASE("setLayers(handle, 0) hides a renderable without touching destroy()/handle validity [gate row]") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::RenderableHandle h = scene.createRenderable(rx::scene::RenderableDesc{});

    scene.setLayers(h, 0U);
    CHECK(scene.layers(h) == 0U);
    CHECK(scene.isRenderableAlive(h));
    CHECK_NOTHROW(static_cast<void>(scene.transform(h)));

    scene.destroyRenderable(h);
    CHECK_FALSE(scene.isRenderableAlive(h));
}

TEST_CASE("Benchmark: 30,000 Scene::setTransform() calls, wall-clock published [no-dirty-tracking criterion]") {
    // A trivial O(1) provider isolates Scene's OWN setTransform overhead
    // from whatever a real asset::Registry lookup would cost -- production
    // wiring is meshBoundsFromRegistry(); this measures Scene's own
    // mechanism.
    const rx::asset::AABB fixed = unitCubeBounds();
    auto bounds = [&fixed](rx::asset::MeshHandle) { return fixed; };
    rx::scene::Scene scene(bounds);

    constexpr uint32_t kCount = 30'000;
    std::vector<rx::scene::RenderableHandle> handles;
    handles.reserve(kCount);
    const rx::scene::RenderableDesc desc;
    for (uint32_t i = 0; i < kCount; ++i) {
        handles.push_back(scene.createRenderable(desc));
    }

    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kCount; ++i) {
        const glm::mat4 t = glm::translate(glm::mat4(1.0F), glm::vec3(static_cast<float>(i), 0.0F, 0.0F));
        scene.setTransform(handles[i], t);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);

    // Published trend number [D18] -- MESSAGE only, never CI-blocking on
    // its own, matching rx_asset/tests/async_import_test.cpp's identical
    // "trend metric... reported via MESSAGE only" posture.
    const double usPerCall = static_cast<double>(elapsed.count()) / static_cast<double>(kCount);
    const double callsPerSec = usPerCall > 0.0 ? 1'000'000.0 / usPerCall : 0.0;
    MESSAGE("rx_scene benchmark: ", kCount, " Scene::setTransform() calls took ", elapsed.count(), " us total (",
            usPerCall, " us/call average, ", callsPerSec, " calls/sec)");

    // Stall detector only [RC6 two-tier pattern], not a tight perf gate:
    // a real O(1)-per-call implementation clears this by roughly two
    // orders of magnitude on any machine this runs on -- the defect class
    // this catches (an accidental O(n) or O(n^2) full-scene rescan hidden
    // inside setTransform()) blows through it by design.
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE(
    "Scene light handle lifecycle mirrors renderables: creation, generational reuse, loud stale-handle failure") {
    rx::scene::Scene scene(&fallbackShapedBounds);

    rx::scene::DirectionalLightDesc desc;
    rx::scene::LightHandle l0 = scene.createDirectionalLight(desc);
    rx::scene::LightHandle l1 = scene.createDirectionalLight(desc);
    CHECK(l0.generation() == 1);
    CHECK(l1.generation() == 1);
    CHECK(scene.lightCount() == 2);

    scene.destroyLight(l0);
    CHECK(scene.lightCount() == 1);
    CHECK_FALSE(scene.isLightAlive(l0));

    rx::scene::LightHandle l0Again = scene.createDirectionalLight(desc);
    CHECK(l0Again.index() == l0.index());
    CHECK(l0Again.generation() == 2);

    CHECK_THROWS_AS(static_cast<void>(scene.lightDirection(l0)), std::out_of_range);
    CHECK_NOTHROW(static_cast<void>(scene.lightDirection(l0Again)));
    CHECK_NOTHROW(scene.destroyLight(l0));  // idempotent double-destroy
}

TEST_CASE("DirectionalLightDesc defaults and field round-trip; setLightChannels [matrix-issue07 API parity]") {
    rx::scene::DirectionalLightDesc desc;
    CHECK(desc.dir == glm::vec3(0.0F, -1.0F, 0.0F));
    CHECK(desc.colorLux == glm::vec3(1.0F, 1.0F, 1.0F));
    CHECK(desc.castsShadows);
    CHECK(desc.channels == 0xFFU);

    desc.dir = glm::vec3(1.0F, 0.0F, 0.0F);
    desc.colorLux = glm::vec3(2.0F, 3.0F, 4.0F);
    desc.castsShadows = false;
    desc.channels = 0x3U;

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::LightHandle h = scene.createDirectionalLight(desc);
    CHECK(scene.lightDirection(h) == glm::vec3(1.0F, 0.0F, 0.0F));
    CHECK(scene.lightColorLux(h) == glm::vec3(2.0F, 3.0F, 4.0F));
    CHECK_FALSE(scene.lightCastsShadows(h));
    CHECK(scene.lightChannels(h) == 0x3U);

    scene.setLightChannels(h, 0x7U);
    CHECK(scene.lightChannels(h) == 0x7U);
}

TEST_CASE(
    "LightManager's SoA storage is sized/typed for punctual (Point/Spot) fields without truncation or "
    "reinterpretation [FG2, issue #5 2026-08-11 comment]") {
    rx::scene::Scene scene(&fallbackShapedBounds);

    rx::scene::LightRecord pointLight;
    pointLight.type = rx::scene::LightType::Point;
    pointLight.position = glm::vec3(1.5F, -2.5F, 3.5F);
    pointLight.colorLux = glm::vec3(10.0F, 20.0F, 30.0F);
    pointLight.range = 12.25F;
    pointLight.innerConeAngle = 0.1F;
    pointLight.outerConeAngle = 0.7F;
    pointLight.castsShadows = false;
    pointLight.channels = 0x5U;

    rx::scene::LightHandle h = rx::scene::detail::createLightRecordForTesting(scene, pointLight);
    const rx::scene::LightRecord& stored = rx::scene::detail::lightRecordForTesting(scene, h);

    // Exact, field-for-field round trip -- no truncation, no
    // reinterpretation.
    CHECK(stored == pointLight);
    CHECK(stored.type == rx::scene::LightType::Point);
    CHECK(stored.position.x == doctest::Approx(1.5F));
    CHECK(stored.range == doctest::Approx(12.25F));
    CHECK(stored.innerConeAngle == doctest::Approx(0.1F));
    CHECK(stored.outerConeAngle == doctest::Approx(0.7F));

    // Coexists cleanly, in the SAME storage, alongside a real, publicly
    // created Directional light.
    rx::scene::LightHandle dirHandle = scene.createDirectionalLight(rx::scene::DirectionalLightDesc{});
    CHECK(scene.lightCount() == 2);
    CHECK(rx::scene::detail::lightRecordForTesting(scene, dirHandle).type == rx::scene::LightType::Directional);
}

// ---------------------------------------------------------------------
// Punctual lights [Phase 5 Stage 2 Task 13, #49] -- the gate matrix's own
// "Point/spot lights have NO public Scene creation API" row, closed:
// createPointLight()/createSpotLight() mirror createDirectionalLight()'s
// own shape+conventions exactly (matrix's own recommendation).
// ---------------------------------------------------------------------

TEST_CASE("PointLightDesc defaults and field round-trip through createPointLight() [gate matrix 'Point/spot "
          "lights have NO public Scene creation API' row]") {
    rx::scene::PointLightDesc desc;
    CHECK(desc.position == glm::vec3(0.0F, 0.0F, 0.0F));
    CHECK(desc.colorCandela == glm::vec3(1.0F, 1.0F, 1.0F));
    CHECK(desc.range == doctest::Approx(0.0F));
    CHECK(desc.castsShadows);
    CHECK(desc.channels == 0xFFU);

    desc.position = glm::vec3(2.0F, 4.0F, 6.0F);
    desc.colorCandela = glm::vec3(1500.0F, 100.0F, 50.0F);  // deliberately unequal per-channel, non-neutral.
    desc.range = 25.0F;
    desc.castsShadows = false;
    desc.channels = 0x2U;

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::LightHandle h = scene.createPointLight(desc);
    CHECK(scene.lightType(h) == rx::scene::LightType::Point);
    CHECK(scene.lightPosition(h) == glm::vec3(2.0F, 4.0F, 6.0F));
    // EXACT pass-through -- candela, no conversion (matches the gate
    // matrix's own "no lumen->candela conversion at import" acceptance
    // criterion, exercised here at the Scene-API level directly).
    CHECK(scene.lightColorLux(h) == glm::vec3(1500.0F, 100.0F, 50.0F));
    CHECK(scene.lightRange(h) == doctest::Approx(25.0F));
    CHECK_FALSE(scene.lightCastsShadows(h));
    CHECK(scene.lightChannels(h) == 0x2U);
}

TEST_CASE("SpotLightDesc defaults (KHR-spec cone-angle defaults 0/PI-over-4) and field round-trip through "
          "createSpotLight()") {
    rx::scene::SpotLightDesc desc;
    CHECK(desc.position == glm::vec3(0.0F, 0.0F, 0.0F));
    CHECK(desc.dir == glm::vec3(0.0F, 0.0F, -1.0F));
    CHECK(desc.colorCandela == glm::vec3(1.0F, 1.0F, 1.0F));
    CHECK(desc.range == doctest::Approx(0.0F));
    CHECK(desc.innerConeAngle == doctest::Approx(0.0F));
    CHECK(desc.outerConeAngle == doctest::Approx(0.7853981634F).epsilon(0.0001));  // PI/4 [KHR spec default].

    desc.position = glm::vec3(1.0F, 2.0F, 3.0F);
    desc.dir = glm::normalize(glm::vec3(1.0F, -1.0F, 0.0F));
    desc.colorCandela = glm::vec3(200.0F, 200.0F, 200.0F);
    desc.range = 5.0F;
    desc.innerConeAngle = 0.2F;
    desc.outerConeAngle = 0.6F;
    desc.channels = 0x9U;

    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::LightHandle h = scene.createSpotLight(desc);
    CHECK(scene.lightType(h) == rx::scene::LightType::Spot);
    CHECK(scene.lightPosition(h) == glm::vec3(1.0F, 2.0F, 3.0F));
    CHECK(scene.lightDirection(h).x == doctest::Approx(desc.dir.x));
    CHECK(scene.lightDirection(h).y == doctest::Approx(desc.dir.y));
    CHECK(scene.lightColorLux(h) == glm::vec3(200.0F, 200.0F, 200.0F));
    CHECK(scene.lightRange(h) == doctest::Approx(5.0F));
    CHECK(scene.lightInnerConeAngle(h) == doctest::Approx(0.2F));
    CHECK(scene.lightOuterConeAngle(h) == doctest::Approx(0.6F));
    CHECK(scene.lightChannels(h) == 0x9U);
}

TEST_CASE("Directional/Point/Spot lights coexist in Scene's own light storage, independently destroyable, "
          "lightCount() exact [growing rx_scene lights to the charter's full punctual set, ticket #49's "
          "own scope line]") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::LightHandle dir = scene.createDirectionalLight(rx::scene::DirectionalLightDesc{});
    rx::scene::LightHandle point = scene.createPointLight(rx::scene::PointLightDesc{});
    rx::scene::LightHandle spot = scene.createSpotLight(rx::scene::SpotLightDesc{});
    CHECK(scene.lightCount() == 3);
    CHECK(scene.lightType(dir) == rx::scene::LightType::Directional);
    CHECK(scene.lightType(point) == rx::scene::LightType::Point);
    CHECK(scene.lightType(spot) == rx::scene::LightType::Spot);

    scene.destroyLight(point);
    CHECK(scene.lightCount() == 2);
    CHECK_FALSE(scene.isLightAlive(point));
    CHECK(scene.isLightAlive(dir));
    CHECK(scene.isLightAlive(spot));
}

TEST_CASE("Scene::lightRecordsSpan()/lightAliveSpan() [Phase 5 Stage 2 Task 14, #50] correctly "
          "distinguish a live slot from a destroyLight()d hole, mirroring Task 19's own "
          "aliveSpan()/generationsSpan() proof for renderables -- necessary now that rx_cluster's "
          "per-frame froxel light-count/scatter passes enumerate every live point/spot light in "
          "BULK, by slot index, rather than through per-light handles") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::PointLightDesc desc;
    desc.colorCandela = glm::vec3(11.0F, 22.0F, 33.0F);
    rx::scene::LightHandle first = scene.createPointLight(desc);
    desc.colorCandela = glm::vec3(44.0F, 55.0F, 66.0F);
    rx::scene::LightHandle second = scene.createPointLight(desc);
    desc.colorCandela = glm::vec3(77.0F, 88.0F, 99.0F);
    rx::scene::LightHandle third = scene.createPointLight(desc);
    (void)first;
    (void)third;

    REQUIRE(scene.lightRecordsSpan().size() == 3);
    REQUIRE(scene.lightAliveSpan().size() == 3);
    // Index i in both spans addresses the SAME slot -- exact value proof,
    // not just size/liveness.
    CHECK(scene.lightAliveSpan()[1] != 0);
    CHECK(scene.lightRecordsSpan()[1].colorLux == glm::vec3(44.0F, 55.0F, 66.0F));

    scene.destroyLight(second);
    // The destroyed slot reads dead but its stale LightRecord content is
    // NOT retroactively zeroed (same "holds whatever its last occupant
    // left" convention aliveSpan()'s own comment documents for renderables)
    // -- a caller MUST gate on lightAliveSpan()[i] before trusting
    // lightRecordsSpan()[i], exactly what this assertion proves.
    CHECK(scene.lightAliveSpan()[1] == 0);
    CHECK(scene.lightRecordsSpan()[1].colorLux == glm::vec3(44.0F, 55.0F, 66.0F));
    CHECK(scene.lightAliveSpan()[0] != 0);
    CHECK(scene.lightAliveSpan()[2] != 0);
}

// ---------------------------------------------------------------------
// instantiateImportedLights() [Phase 5 Stage 2 Task 13, #49] -- converts
// asset::LightData (already parsed+preserved since Phase 4, per
// import_gltf.cpp's own KHR_lights_punctual extension parse) into real
// Scene lights. This device-free TEST_CASE hand-builds LightData directly
// (no GPU-backed importGltf() needed to exercise the CONVERSION logic
// itself -- the real end-to-end fastgltf-parse-through-Scene path is
// covered separately, GPU-backed, in
// src/rx_asset/tests/import_gltf_gpu_test.cpp, reusing this exact
// fixture's own committed asset -- assets/test/cube_lights_camera.gltf).
// ---------------------------------------------------------------------

TEST_CASE("instantiateImportedLights(): value-asserted decoded-value discipline -- EXACT candela/lux "
          "pass-through (no unit conversion), position/direction derived from the node's own world "
          "transform, KHR cone-angle defaults applied when absent [gate matrix 'Authored glTF punctual "
          "lights arrive in the Scene with value-asserted intensities/cones' acceptance criterion]") {
    std::vector<rx::asset::LightData> lights;

    // Directional -- translated+rotated node (translation must NOT affect
    // direction; only rotation does, per KHR spec). 90-degree rotation
    // about +Y maps local (0,0,-1) to world (-1,0,0).
    {
        rx::asset::LightData ld;
        ld.type = rx::asset::LightData::Type::Directional;
        ld.color = glm::vec3(1.0F, 0.9F, 0.8F);
        ld.intensity = 3.0F;
        ld.worldTransform =
            glm::translate(glm::mat4(1.0F), glm::vec3(100.0F, 200.0F, 300.0F)) *
            glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        lights.push_back(ld);
    }
    // Point -- translation only, no rotation; range PRESENT.
    {
        rx::asset::LightData ld;
        ld.type = rx::asset::LightData::Type::Point;
        ld.color = glm::vec3(1.0F, 0.0F, 0.0F);
        ld.intensity = 1500.0F;  // candela [gate matrix's own worked example value].
        ld.range = 10.0F;
        ld.worldTransform = glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 3.0F, 4.0F));
        lights.push_back(ld);
    }
    // Spot -- range/cone angles ABSENT (nullopt) -- must resolve to the
    // KHR spec's own documented defaults, not zero/garbage.
    {
        rx::asset::LightData ld;
        ld.type = rx::asset::LightData::Type::Spot;
        ld.color = glm::vec3(0.0F, 1.0F, 0.0F);
        ld.intensity = 200.0F;
        ld.worldTransform = glm::translate(glm::mat4(1.0F), glm::vec3(5.0F, 6.0F, 7.0F));
        // ld.range / innerConeAngle / outerConeAngle left std::nullopt.
        lights.push_back(ld);
    }

    rx::scene::Scene scene(&fallbackShapedBounds);
    std::vector<rx::scene::LightHandle> handles = rx::scene::instantiateImportedLights(scene, lights);
    REQUIRE(handles.size() == 3);
    CHECK(scene.lightCount() == 3);

    // Directional: colorLux == color*intensity EXACTLY (lux, no
    // conversion); direction reflects the 90-degree-about-Y rotation
    // (translation has zero effect on direction).
    CHECK(scene.lightType(handles[0]) == rx::scene::LightType::Directional);
    const glm::vec3 dirColor = scene.lightColorLux(handles[0]);
    CHECK(dirColor.x == doctest::Approx(3.0F));
    CHECK(dirColor.y == doctest::Approx(2.7F));
    CHECK(dirColor.z == doctest::Approx(2.4F));
    const glm::vec3 dirDirection = scene.lightDirection(handles[0]);
    CHECK(dirDirection.x == doctest::Approx(-1.0F).epsilon(0.001));
    CHECK(dirDirection.y == doctest::Approx(0.0F).epsilon(0.001));
    CHECK(dirDirection.z == doctest::Approx(0.0F).epsilon(0.001));

    // Point: colorCandela == color*intensity EXACTLY (candela, no
    // conversion -- the gate matrix's own "1500 candela imports as
    // EXACTLY 1500.0, not 1500/(4*pi)" acceptance criterion); position ==
    // translation; range == the PRESENT value (not the "absent" sentinel).
    CHECK(scene.lightType(handles[1]) == rx::scene::LightType::Point);
    const glm::vec3 pointColor = scene.lightColorLux(handles[1]);
    CHECK(pointColor.x == doctest::Approx(1500.0F));
    CHECK(pointColor.y == doctest::Approx(0.0F));
    CHECK(pointColor.z == doctest::Approx(0.0F));
    CHECK(scene.lightPosition(handles[1]) == glm::vec3(2.0F, 3.0F, 4.0F));
    CHECK(scene.lightRange(handles[1]) == doctest::Approx(10.0F));

    // Spot: range/cone-angle absence resolves to the KHR spec's own
    // documented defaults (0 / PI/4), matching SpotLightDesc's own
    // default-member-initializer values exactly -- NOT left at 0/0 (which
    // would be a degenerate, always-fully-attenuated cone).
    CHECK(scene.lightType(handles[2]) == rx::scene::LightType::Spot);
    const glm::vec3 spotColor = scene.lightColorLux(handles[2]);
    CHECK(spotColor.x == doctest::Approx(0.0F));
    CHECK(spotColor.y == doctest::Approx(200.0F));
    CHECK(spotColor.z == doctest::Approx(0.0F));
    CHECK(scene.lightPosition(handles[2]) == glm::vec3(5.0F, 6.0F, 7.0F));
    CHECK(scene.lightRange(handles[2]) == doctest::Approx(0.0F));  // absent -> "no configured range" sentinel.
    CHECK(scene.lightInnerConeAngle(handles[2]) == doctest::Approx(0.0F));
    CHECK(scene.lightOuterConeAngle(handles[2]) == doctest::Approx(0.7853981634F).epsilon(0.0001));
}

TEST_CASE("instantiateImportedLights(): empty input produces zero lights (no accidental default light "
          "injected) -- discriminates against a scene with no embedded KHR_lights_punctual data (e.g. "
          "Sponza) silently gaining a phantom light.") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    std::vector<rx::scene::LightHandle> handles = rx::scene::instantiateImportedLights(scene, {});
    CHECK(handles.empty());
    CHECK(scene.lightCount() == 0);
}

// ---------------------------------------------------------------------
// Environment [Phase 5 Task 10, #46, FG1 closure]
// ---------------------------------------------------------------------

TEST_CASE("Scene environment: absent by default, hasEnvironment()/environment() round-trip a set value, "
          "environment() throws for an unconfigured Scene [gate matrix 'Scene-level environment API' row]") {
    rx::scene::Scene scene(&fallbackShapedBounds);

    // Absent by default -- a freshly-constructed Scene has no environment
    // (the "existing 08/09 gates regenerate with new, provably-more-
    // correct references" bar, not "byte-identical unless requested" --
    // see this class's own EnvironmentDesc header comment).
    CHECK_FALSE(scene.hasEnvironment());
    CHECK_THROWS_AS(static_cast<void>(scene.environment()), std::out_of_range);

    rx::scene::EnvironmentDesc desc;
    desc.baseCubemapIndex = 3;
    desc.irradianceCubemapIndex = 4;
    desc.prefilteredCubemapIndex = 5;
    desc.dfgLutIndex = 6;
    desc.cubeSamplerIndex = 1;
    desc.dfgSamplerIndex = 2;
    desc.maxPrefilteredLod = 4.0F;
    desc.intensity = 2.5F;
    scene.setEnvironment(desc);

    REQUIRE(scene.hasEnvironment());
    const rx::scene::EnvironmentDesc& stored = scene.environment();
    CHECK(stored.baseCubemapIndex == 3);
    CHECK(stored.irradianceCubemapIndex == 4);
    CHECK(stored.prefilteredCubemapIndex == 5);
    CHECK(stored.dfgLutIndex == 6);
    CHECK(stored.cubeSamplerIndex == 1);
    CHECK(stored.dfgSamplerIndex == 2);
    CHECK(stored.maxPrefilteredLod == doctest::Approx(4.0F));
    CHECK(stored.intensity == doctest::Approx(2.5F));
}

TEST_CASE("Scene environment: setEnvironment() REPLACES a prior value (singleton, not a handle pool), "
          "clearEnvironment() removes it again") {
    rx::scene::Scene scene(&fallbackShapedBounds);

    rx::scene::EnvironmentDesc first;
    first.baseCubemapIndex = 10;
    first.intensity = 1.0F;
    scene.setEnvironment(first);
    REQUIRE(scene.hasEnvironment());
    CHECK(scene.environment().baseCubemapIndex == 10);

    rx::scene::EnvironmentDesc second;
    second.baseCubemapIndex = 20;
    second.intensity = 3.0F;
    scene.setEnvironment(second);
    REQUIRE(scene.hasEnvironment());
    // A real REPLACE, not an additive/accumulating set -- exactly one
    // active environment at a time, matching this struct's own singleton
    // rationale (Filament's `setIndirectLight()`/`setSkybox()` precedent).
    CHECK(scene.environment().baseCubemapIndex == 20);
    CHECK(scene.environment().intensity == doctest::Approx(3.0F));

    scene.clearEnvironment();
    CHECK_FALSE(scene.hasEnvironment());
    CHECK_THROWS_AS(static_cast<void>(scene.environment()), std::out_of_range);

    // clearEnvironment() on an already-clear Scene is a harmless no-op
    // (not a throw) -- mirrors destroyRenderable()/destroyLight()'s own
    // idempotent-mutator convention for this class.
    scene.clearEnvironment();
    CHECK_FALSE(scene.hasEnvironment());
}
