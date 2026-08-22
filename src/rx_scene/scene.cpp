#include <rx_scene/scene.h>

#include <rx_asset/registry.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>

#include <stdexcept>

namespace rx::scene {

MeshBoundsFn meshBoundsFromRegistry(const asset::Registry& registry) {
    // See scene.h's own "MESH BOUNDS RESOLUTION" comment: forwards
    // verbatim to Registry::mesh(handle).bounds, which is ALREADY D24
    // residency-tolerant (a dead/evicted handle resolves to the fallback
    // mesh's own [zero-volume] bounds through Registry itself, never a
    // null dereference and never an assert) -- no logic duplicated here.
    return [&registry](asset::MeshHandle mesh) -> asset::AABB { return registry.mesh(mesh).bounds; };
}

Scene::Scene(MeshBoundsFn meshBounds) : meshBounds_(std::move(meshBounds)) {}

Scene::~Scene() = default;

// ---------------------------------------------------------------------
// Renderables
// ---------------------------------------------------------------------

bool Scene::isLiveRenderableIndex(RenderableHandle handle) const {
    return handle.index() < generation_.size() && alive_[handle.index()] && generation_[handle.index()] == handle.generation();
}

uint32_t Scene::requireLiveRenderable(RenderableHandle handle, const char* context) const {
    if (!isLiveRenderableIndex(handle)) {
        RX_LOG_ERROR("{}: unknown or stale RenderableHandle (index={}, generation={})", context, handle.index(),
                      handle.generation());
        throw std::out_of_range(context);
    }
    return handle.index();
}

void Scene::recomputeWorldBounds(uint32_t index) {
    // D24-at-the-proxy-level: re-invokes meshBounds_ fresh every call --
    // see scene.h's own top comment. `transformed()` (rx_asset/mesh_asset.h)
    // transforms all 8 corners of the local AABB, correct under rotation
    // and negative scale.
    worldBounds_[index] = meshBounds_(mesh_[index]).transformed(transform_[index]);
}

RenderableHandle Scene::createRenderable(const RenderableDesc& desc) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::createRenderable");
    RX_ZONE;

    uint32_t idx = 0;
    uint32_t generation = 0;
    if (!freeList_.empty()) {
        idx = freeList_.back();
        freeList_.pop_back();
        generation = ++generation_[idx];
    } else {
        idx = static_cast<uint32_t>(generation_.size());
        generation = 1;
        generation_.push_back(generation);
        alive_.push_back(false);
        mesh_.emplace_back();
        submeshOverrides_.emplace_back();
        transform_.emplace_back(1.0F);
        worldBounds_.emplace_back();
        layers_.push_back(0);
        channels_.push_back(0);
        castsShadows_.push_back(0);
        priority_.push_back(0);
        skinningBufferIndex_.emplace_back();
        morphTargetBufferIndex_.emplace_back();
    }
    alive_[idx] = true;

    uint8_t clampedPriority = desc.priority;
    if (clampedPriority > 7) {
        RX_LOG_WARN("rx::scene::Scene::createRenderable: priority {} out of [0,7], clamped to 7",
                     static_cast<int>(desc.priority));
        clampedPriority = 7;
    }

    mesh_[idx] = desc.mesh;
    submeshOverrides_[idx].assign(desc.submeshMaterialOverrides.begin(), desc.submeshMaterialOverrides.end());
    transform_[idx] = desc.transform;
    layers_[idx] = desc.layers;
    channels_[idx] = desc.channels;
    castsShadows_[idx] = desc.castsShadows ? 1 : 0;
    priority_[idx] = clampedPriority;
    skinningBufferIndex_[idx] = desc.skinningBufferIndex;
    morphTargetBufferIndex_[idx] = desc.morphTargetBufferIndex;

    recomputeWorldBounds(idx);

    return RenderableHandle(idx, generation);
}

void Scene::destroyRenderable(RenderableHandle handle) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::destroyRenderable");
    if (!isLiveRenderableIndex(handle)) {
        RX_LOG_WARN("rx::scene::Scene::destroyRenderable: handle (index={}, generation={}) already dead or unknown, ignored",
                     handle.index(), handle.generation());
        return;
    }
    const uint32_t idx = handle.index();
    alive_[idx] = false;
    freeList_.push_back(idx);
    // Release the one heap-allocating column promptly rather than waiting
    // for reuse to overwrite it -- every other column is a fixed-size POD
    // row with nothing to free.
    submeshOverrides_[idx].clear();
    submeshOverrides_[idx].shrink_to_fit();
}

void Scene::setTransform(RenderableHandle handle, const glm::mat4& transform) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::setTransform");
    RX_ZONE;
    const uint32_t idx = requireLiveRenderable(handle, "rx::scene::Scene::setTransform");
    // O(1) direct SoA write -- see scene.h's own "NO DIRTY TRACKING"
    // comment; no dirty bit, no propagation pass.
    transform_[idx] = transform;
    recomputeWorldBounds(idx);
}

void Scene::setLayers(RenderableHandle handle, uint32_t layers) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::setLayers");
    const uint32_t idx = requireLiveRenderable(handle, "rx::scene::Scene::setLayers");
    layers_[idx] = layers;
}

void Scene::setChannels(RenderableHandle handle, uint8_t channels) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::setChannels");
    const uint32_t idx = requireLiveRenderable(handle, "rx::scene::Scene::setChannels");
    channels_[idx] = channels;
}

bool Scene::isRenderableAlive(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::isRenderableAlive");
    return isLiveRenderableIndex(handle);
}

const glm::mat4& Scene::transform(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::transform");
    return transform_[requireLiveRenderable(handle, "rx::scene::Scene::transform")];
}

const asset::AABB& Scene::worldBounds(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::worldBounds");
    return worldBounds_[requireLiveRenderable(handle, "rx::scene::Scene::worldBounds")];
}

uint32_t Scene::layers(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::layers");
    return layers_[requireLiveRenderable(handle, "rx::scene::Scene::layers")];
}

uint8_t Scene::channels(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::channels");
    return channels_[requireLiveRenderable(handle, "rx::scene::Scene::channels")];
}

bool Scene::castsShadows(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::castsShadows");
    return castsShadows_[requireLiveRenderable(handle, "rx::scene::Scene::castsShadows")] != 0;
}

uint8_t Scene::priority(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::priority");
    return priority_[requireLiveRenderable(handle, "rx::scene::Scene::priority")];
}

asset::MeshHandle Scene::mesh(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::mesh");
    return mesh_[requireLiveRenderable(handle, "rx::scene::Scene::mesh")];
}

std::span<const std::optional<asset::MaterialHandle>> Scene::submeshMaterialOverrides(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::submeshMaterialOverrides");
    return submeshOverrides_[requireLiveRenderable(handle, "rx::scene::Scene::submeshMaterialOverrides")];
}

std::optional<uint32_t> Scene::skinningBufferIndex(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::skinningBufferIndex");
    return skinningBufferIndex_[requireLiveRenderable(handle, "rx::scene::Scene::skinningBufferIndex")];
}

std::optional<uint32_t> Scene::morphTargetBufferIndex(RenderableHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::morphTargetBufferIndex");
    return morphTargetBufferIndex_[requireLiveRenderable(handle, "rx::scene::Scene::morphTargetBufferIndex")];
}

size_t Scene::renderableCount() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::renderableCount");
    // Every slot is either on freeList_ (released) or live -- mirrors
    // rx::core::HandlePool::liveCount()'s own identical O(1) derivation.
    return generation_.size() - freeList_.size();
}

std::span<const glm::mat4> Scene::transformsSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::transformsSpan");
    return transform_;
}

std::span<const asset::AABB> Scene::worldBoundsSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::worldBoundsSpan");
    return worldBounds_;
}

std::span<const uint32_t> Scene::layersSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::layersSpan");
    return layers_;
}

std::span<const uint8_t> Scene::channelsSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::channelsSpan");
    return channels_;
}

std::span<const uint8_t> Scene::castsShadowsSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::castsShadowsSpan");
    return castsShadows_;
}

std::span<const uint8_t> Scene::prioritySpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::prioritySpan");
    return priority_;
}

std::span<const asset::MeshHandle> Scene::meshSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::meshSpan");
    return mesh_;
}

std::span<const uint8_t> Scene::aliveSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::aliveSpan");
    return alive_;
}

std::span<const uint32_t> Scene::generationsSpan() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::generationsSpan");
    return generation_;
}

// ---------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------

bool Scene::isLiveLightIndex(LightHandle handle) const {
    return handle.index() < lightGeneration_.size() && lightAlive_[handle.index()] &&
           lightGeneration_[handle.index()] == handle.generation();
}

uint32_t Scene::requireLiveLight(LightHandle handle, const char* context) const {
    if (!isLiveLightIndex(handle)) {
        RX_LOG_ERROR("{}: unknown or stale LightHandle (index={}, generation={})", context, handle.index(),
                      handle.generation());
        throw std::out_of_range(context);
    }
    return handle.index();
}

LightHandle Scene::insertLightRecord(const LightRecord& record) {
    uint32_t idx = 0;
    uint32_t generation = 0;
    if (!lightFreeList_.empty()) {
        idx = lightFreeList_.back();
        lightFreeList_.pop_back();
        generation = ++lightGeneration_[idx];
    } else {
        idx = static_cast<uint32_t>(lightGeneration_.size());
        generation = 1;
        lightGeneration_.push_back(generation);
        lightAlive_.push_back(false);
        lightRecords_.emplace_back();
    }
    lightAlive_[idx] = true;
    lightRecords_[idx] = record;
    return LightHandle(idx, generation);
}

LightHandle Scene::createDirectionalLight(const DirectionalLightDesc& desc) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::createDirectionalLight");
    LightRecord record;
    record.type = LightType::Directional;
    record.direction = desc.dir;
    record.colorLux = desc.colorLux;
    record.castsShadows = desc.castsShadows;
    record.channels = desc.channels;
    // position/range/innerConeAngle/outerConeAngle stay at LightRecord's
    // own inert defaults [FG2] -- this is the only public light-creation
    // path in Phase 4, and it never populates the punctual-only fields.
    return insertLightRecord(record);
}

LightHandle Scene::createPointLight(const PointLightDesc& desc) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::createPointLight");
    LightRecord record;
    record.type = LightType::Point;
    record.position = desc.position;
    record.colorLux = desc.colorCandela;  // candela -- see PointLightDesc's own header comment.
    record.range = desc.range;
    record.castsShadows = desc.castsShadows;
    record.channels = desc.channels;
    // direction/innerConeAngle/outerConeAngle stay at LightRecord's own
    // inert defaults -- a point light emits uniformly in every direction.
    return insertLightRecord(record);
}

LightHandle Scene::createSpotLight(const SpotLightDesc& desc) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::createSpotLight");
    LightRecord record;
    record.type = LightType::Spot;
    record.direction = desc.dir;
    record.position = desc.position;
    record.colorLux = desc.colorCandela;  // candela -- see SpotLightDesc's own header comment.
    record.range = desc.range;
    record.innerConeAngle = desc.innerConeAngle;
    record.outerConeAngle = desc.outerConeAngle;
    record.castsShadows = desc.castsShadows;
    record.channels = desc.channels;
    return insertLightRecord(record);
}

void Scene::destroyLight(LightHandle handle) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::destroyLight");
    if (!isLiveLightIndex(handle)) {
        RX_LOG_WARN("rx::scene::Scene::destroyLight: handle (index={}, generation={}) already dead or unknown, ignored",
                     handle.index(), handle.generation());
        return;
    }
    const uint32_t idx = handle.index();
    lightAlive_[idx] = false;
    lightFreeList_.push_back(idx);
}

void Scene::setLightChannels(LightHandle handle, uint8_t channels) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::setLightChannels");
    lightRecords_[requireLiveLight(handle, "rx::scene::Scene::setLightChannels")].channels = channels;
}

bool Scene::isLightAlive(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::isLightAlive");
    return isLiveLightIndex(handle);
}

LightType Scene::lightType(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightType");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightType")].type;
}

glm::vec3 Scene::lightDirection(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightDirection");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightDirection")].direction;
}

glm::vec3 Scene::lightPosition(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightPosition");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightPosition")].position;
}

glm::vec3 Scene::lightColorLux(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightColorLux");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightColorLux")].colorLux;
}

float Scene::lightRange(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightRange");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightRange")].range;
}

float Scene::lightInnerConeAngle(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightInnerConeAngle");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightInnerConeAngle")].innerConeAngle;
}

float Scene::lightOuterConeAngle(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightOuterConeAngle");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightOuterConeAngle")].outerConeAngle;
}

bool Scene::lightCastsShadows(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightCastsShadows");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightCastsShadows")].castsShadows;
}

uint8_t Scene::lightChannels(LightHandle handle) const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightChannels");
    return lightRecords_[requireLiveLight(handle, "rx::scene::Scene::lightChannels")].channels;
}

size_t Scene::lightCount() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::lightCount");
    return lightGeneration_.size() - lightFreeList_.size();
}

// ---------------------------------------------------------------------
// Environment [Phase 5 Task 10, #46]
// ---------------------------------------------------------------------

void Scene::setEnvironment(const EnvironmentDesc& desc) {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::setEnvironment");
    environment_ = desc;
}

void Scene::clearEnvironment() {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::clearEnvironment");
    environment_.reset();
}

bool Scene::hasEnvironment() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::hasEnvironment");
    return environment_.has_value();
}

const EnvironmentDesc& Scene::environment() const {
    RX_ASSERT_MAIN_THREAD("rx::scene::Scene::environment");
    if (!environment_.has_value()) {
        RX_LOG_ERROR("rx::scene::Scene::environment: no environment configured -- call hasEnvironment() first");
        throw std::out_of_range("rx::scene::Scene::environment");
    }
    return *environment_;
}

// ---------------------------------------------------------------------
// Imported-light consumption [Phase 5 Stage 2 Task 13, #49]
// ---------------------------------------------------------------------

std::vector<LightHandle> instantiateImportedLights(Scene& scene, std::span<const asset::LightData> lights) {
    RX_ZONE;
    std::vector<LightHandle> handles;
    handles.reserve(lights.size());
    for (const asset::LightData& light : lights) {
        // Translation column [KHR spec: "the light's position is defined
        // as the node's world location"].
        const glm::vec3 position(light.worldTransform[3]);
        // Local (0,0,-1) rotated by the node's world transform [KHR spec:
        // "an untransformed light points down the -Z axis"] -- `mat4 *
        // vec4(0,0,-1,0)` (a DIRECTION, w=0, so translation drops out)
        // picks out exactly `-worldTransform`'s own third column, which is
        // correct under non-uniform scale on the OTHER two axes too (the
        // matrix-vector product never touches column 0/1 at all) --
        // normalizing removes whatever scale factor local Z itself
        // carries, matching the spec's own "light properties are
        // unaffected by node transforms" / "inherited scale does not
        // affect cone shape" text (scale affects neither the resulting
        // unit direction nor anything else this function reads).
        const glm::vec3 direction = glm::normalize(glm::vec3(light.worldTransform * glm::vec4(0.0F, 0.0F, -1.0F, 0.0F)));
        // Straight pass-through, no unit conversion [KHR spec: "The
        // intensity represents the luminous intensity... if it were
        // colored pure white"; "color" is "a wavelength-specific
        // multiplier"] -- see this function's own header comment (scene.h)
        // for the full "no lumen->candela rescale" acceptance criterion.
        const glm::vec3 colorIntensity = light.color * light.intensity;

        switch (light.type) {
            case asset::LightData::Type::Directional: {
                DirectionalLightDesc desc;
                desc.dir = direction;
                desc.colorLux = colorIntensity;
                handles.push_back(scene.createDirectionalLight(desc));
                break;
            }
            case asset::LightData::Type::Point: {
                PointLightDesc desc;
                desc.position = position;
                desc.colorCandela = colorIntensity;
                desc.range = light.range.value_or(0.0F);  // absent -> "no configured range" sentinel.
                handles.push_back(scene.createPointLight(desc));
                break;
            }
            case asset::LightData::Type::Spot: {
                SpotLightDesc desc;
                desc.position = position;
                desc.dir = direction;
                desc.colorCandela = colorIntensity;
                desc.range = light.range.value_or(0.0F);
                // KHR spec defaults (README.md): innerConeAngle=0,
                // outerConeAngle=PI/4 -- SAME literals as SpotLightDesc's
                // own default-member-initializers, restated explicitly
                // here since `light.innerConeAngle`/`outerConeAngle` are
                // `std::optional<float>` and this is the exact point that
                // resolves "absent" to the spec's own documented value.
                desc.innerConeAngle = light.innerConeAngle.value_or(0.0F);
                desc.outerConeAngle = light.outerConeAngle.value_or(0.7853981633974483F);
                handles.push_back(scene.createSpotLight(desc));
                break;
            }
        }
    }
    return handles;
}

namespace detail {

LightHandle createLightRecordForTesting(Scene& scene, const LightRecord& record) { return scene.insertLightRecord(record); }

const LightRecord& lightRecordForTesting(const Scene& scene, LightHandle handle) {
    return scene.lightRecords_[scene.requireLiveLight(handle, "rx::scene::detail::lightRecordForTesting")];
}

}  // namespace detail

}  // namespace rx::scene
