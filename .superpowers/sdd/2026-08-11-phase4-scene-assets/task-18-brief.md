### Task 18: Scene proxies (`rx_scene`, D19) + reversed-Z camera (D13)

**Files:** Create `src/rx_scene/{CMakeLists.txt,include/rx_scene/{scene.h,camera.h},scene.cpp,tests/...}`.
**Interfaces (produces):**
```cpp
namespace rx::scene {
struct Camera { /* pos/orientation, vfov, near; reversed-inf-far projection helpers (D13): proj(), viewProj(); cullMask u32 = ~0u */ };
using RenderableHandle = ...; using LightHandle = ...;
struct RenderableDesc { asset::MeshHandle mesh; /* per-submesh material overrides optional */ glm::mat4 transform; uint32_t layers = ~0u; uint8_t channels = 0xFF; };
struct DirectionalLightDesc { glm::vec3 dir; glm::vec3 colorLux; bool castsShadows; uint8_t channels = 0xFF; };
class Scene { // SoA managers inside (transform pool carries prev-frame slot layout per seed-8/temporal note)
  RenderableHandle createRenderable(const RenderableDesc&);
  void setTransform(RenderableHandle, const glm::mat4&); void setLayers(RenderableHandle, uint32_t); /* destroy, light equivalents */
};}
```
Reversed-Z: depth attachment usage in samples migrating in Task 22/24; Camera helpers are the single source of projection truth; unit tests assert near→1/far→0 mapping and frustum plane extraction correctness.
**Steps:** device-free tests (handle lifecycle incl. generational failure, SoA iteration order, prev-transform slot updated on setTransform) → implement → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue05-scene-proxies.md` as amended by
`gate/rulings-2026-08-18.md` §#5 + RC5. Key deltas: reuse
`rx::core::Handle<Tag>` (the TYPE — not the AoS `HandlePool`; storage
is SoA columns per D19, span-accessor test proves it);
`RenderableDesc` gains `castsShadows: bool = true` (RC5) and
`priority: uint8_t` (0-7, default 4 — reserved sort tier, Filament
precedent); per-submesh material override becomes a concrete typed
field (span of optional MaterialHandle), not a comment; reserved
skinning/morph slots (inert, tested); `Camera` gains `cullingProj()`
(separate FINITE culling projection — Gribb-Hartmann degenerates
under D13's infinite far; Filament's own answer) and an inert jitter
offset; exposure STAYS on the tonemap (D22 stands — camera exposure
API registered for the techniques phase with physical units);
no-dirty-tracking is itself a criterion (setTransform = O(1) SoA
write; 30k-call benchmark published); D24 residency-tolerant resolve
test at the proxy level.

