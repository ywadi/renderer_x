# Matrix — Issue #5: Scene submission layer (render proxies)

**Ticket:** #5 "Scene submission layer" (`phase-4`, `stage-2`)
**Plan task:** Task 18, `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:350-366`
("Scene proxies (`rx_scene`, D19) + reversed-Z camera (D13)")
**Binding spec decisions:** D12 (hierarchy flattened at import), D13 (reversed-Z
main camera), D19 (scene data model: Filament-precedent managers, no ECS),
D24 (memory budget/eviction invariant), D26 (per-draw addressing —
consumed by DrawListBuilder but the payload fields originate on the proxy),
D27 (main-thread pre-resolution — consumes proxy-resolved material/mesh
handles).
**Issue amendments (binding):** transform-pool prev-frame-slot design note
(2026-08-10, comment on #5) and LightManager forward-compat sizing note
(2026-08-11, comment on #5, FG2).

**Sources consulted:**
- `gh issue view 5 --json body,comments` (2026-08-18) — ticket body + both
  design-note comments.
- Plan Task 18 interface block, `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:350-366`.
- Spec `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`:
  D12 (:216-223), D13 (:225-234), D19 (:293-305), D24 (:344-367), D26
  (:391-422), D27 (:424-440).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/research-p4-scene.md`
  §1, §5.
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/feature-gap-audit.md`
  gaps #1 (FG1, environment lighting/exposure interaction) and #2 (FG2,
  punctual lights).
- `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:191-226`
  (FG1/FG2/FG9 registry text).
- Filament (google/filament, GitHub, commit `d438927c1179b9abe7ede4efab7e2d4c2858d3e1`,
  fetched 2026-08-18): `filament/include/filament/RenderableManager.h`,
  `TransformManager.h`, `LightManager.h`, `Camera.h`, `Frustum.h`, `View.h`;
  `filament/src/components/RenderableManager.cpp`; `libs/utils/include/utils/Entity.h`,
  `EntityManager.h`.
- In-repo code: `src/rx_core/include/rx_core/handle.h`,
  `src/rx_material/include/rx_material/material_system.h:36-44`,
  `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`.
- Gribb, G. & Hartmann, K., "Fast Extraction of Viewing Frustum Planes from
  the World-View-Projection Matrix" — http://graphics.cs.ucf.edu/cap4720/fall2008/plane_extraction.pdf
  (canonical citation, verified reachable via web search 2026-08-18).
- Reversed-Z/infinite-far precedent: Reed, N., "Depth Precision Visualized",
  https://www.reedbeta.com/blog/depth-precision-visualized/; community GLM
  implementation cross-checked at
  https://gist.github.com/pezcode/1609b61a1eedd207ec8c5acf6f94f53a (fetched
  2026-08-18; not an official library — see Verification health).

---

## The matrix

| Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
|---|---|---|---|---|
| Per-instance bounding-box override | Filament `RenderableManager::Builder::boundingBox(Box)` — object-space AABB, "mandatory unless culling disabled"; runtime `setAxisAlignedBoundingBox(Instance, Box)` (RenderableManager.h:290-296,658). Required because skinning/morphing move vertices beyond the imported mesh's static bounds. | consume-now for the mesh-derived default; preserve-later for an explicit per-instance override field. | UNVERIFIED against RendererX's own asset pipeline: Task 18's `RenderableDesc` (plan:358) has no `boundingBox` field at all — it is silent on where the culling AABB comes from. Whether `asset::MeshHandle`/GeometryPool already stores a per-mesh AABB is Stage-1 (asset-pipeline) scope, not verified in this pass. | Test: `createRenderable` populates the proxy's world-space AABB from the mesh's stored bounds transformed by the initial `transform`; a second test asserts `setTransform` updates the AABB (not just the matrix) before the next `DrawListBuilder::build()` call, since D15's culling reads AABBs directly. |
| Skinning attach point | Filament `Builder::skinning(SkinningBuffer*, boneCount, offset)` (up to 255 bones) + per-frame `setBones()` (RenderableManager.h:403-455,803-820). | preserve-later — issue #5 explicitly scopes this as a "preserve-later hook", not Phase-4 rendering. | N/A (no skinning renderer exists yet; this is a storage-layout question only). | Test: `RenderableDesc`/the internal SoA row reserves a typed slot (e.g. an optional skinning-buffer index defaulted to "none") so the animation phase adds population, not a struct/ABI reshape. Retrofit-economics justification (per gate rule): a later add-on requires touching every `createRenderable` call site's struct literal if the field isn't reserved now — same argument the issue's own LightManager comment already accepts for point/spot lights. |
| Morph-target attach point | Filament `Builder::morphing(MorphTargetBuffer*)` (standard, up to `CONFIG_MAX_MORPH_TARGET_COUNT`) or legacy (≤4 targets) (RenderableManager.h:512-547). | preserve-later — same reasoning as skinning above; issue #5 groups both under "skinning/morph attach points (preserve-later hooks)". | N/A (no morph target renderer exists yet). | Same test shape as skinning: reserved slot, not implemented; a device-free test asserts the slot exists and defaults to inert/no-op. |
| Priority (coarse draw-order override, orthogonal to sort key) | Filament `Builder::priority(uint8_t)` clamped `[0..7]`, default 4, applied separately within the opaque and translucent partitions, explicitly "orthogonal to `layerMask`" (RenderableManager.h:305-339). Also occupies its own 3-bit field in Filament's `RenderPass::CommandKey` (`PRIORITY_MASK`/`PRIORITY_SHIFT=50`, `RenderPass.h:158-159`) — i.e. it is a real top-level sort-key tier, not decoration. | log-don't-drop → new gap (see below); not present anywhere in D14/D19/Task 18. | N/A — no RendererX equivalent exists. | If adopted: `RenderableDesc` gains an optional `priority: uint8_t` (default mid-value); DrawListBuilder's u64 key reserves bits above pipeline/material/depth for it (see matrix-issue06 sort-key row). If deferred: this ticket should explicitly record the deferral (currently it is simply absent, which the gate's own rules treat as a gap unless retrofit economics excuse it — see New gaps). |
| Per-primitive blend order (tie-break within back-to-front sort) | Filament `Builder::blendOrder(primitiveIndex, uint16_t)`, lowest 15 bits used, default 0 (RenderableManager.h:553-563); consumed directly in `RenderPass::CommandKey`'s BLENDED-command low 47 bits (distance + 15-bit blendOrder + 1-bit two-pass flag, `RenderPass.h:100-104`). | log-don't-drop → new gap; D14 only specifies "blend sorted strictly back-to-front by depth" with no secondary key. | N/A — no RendererX equivalent exists; `DrawPayload` (plan:377) has no blend-order field. | Without this, two transparent submeshes on the same object at coincident/near-coincident depth (e.g. layered materials) have an *unstable* relative order across frames/thread counts — directly in tension with D26/D27's own determinism requirement. Minimum acceptance bar if deferred: document that alpha-blended multi-submesh objects with overlapping depth are a known-unordered case in Phase 4, not silently assumed correct. |
| Render "channel" (isolated draw-order grouping, independent of layers/priority — e.g. UI drawn as its own group regardless of state) | Filament `Builder::channel(uint8_t)`, 8 channels `[0..7]` default 2, "All renderables in a given channel are rendered together, regardless of anything else" (RenderableManager.h:342-359); top bits of `CommandKey` (`CHANNEL_SHIFT=61`, `CONFIG_RENDERPASS_CHANNEL_COUNT=8`, `EngineEnums.h:105`) — i.e. the single highest-priority sort criterion. | N/A-Phase-4, justified: RendererX's ImGui overlay (Task 21) is its own declared render-graph pass, not interleaved into `DrawListBuilder`'s scene draws, so Filament's motivating use case (isolate UI/overlay draws inside one command stream) doesn't arise in Phase 4's architecture. Retrofit cost if wrong: low — this would be an additive top sort-key tier, not a restructure, so deferring is cheap. | N/A. | No test needed while N/A; if a future need appears, propose reserving 0 bits now (no action) since the addition is non-breaking later. |
| Layer visibility mask (renderable side) vs camera cull mask | Unity 32-bit `LayerMask`; Godot `VisualInstance3D.layers`/`Camera3D.cull_mask` (32-bit capacity, 20 user-exposed — docs.godotengine.org/en/stable/classes/class_visualinstance3d.html, verified 2026-08-18); Filament `layerMask` is only **8-bit** (`Builder::layerMask(uint8_t select, uint8_t values)`, RenderableManager.h:305-312) — a real divergence between "no-ECS component-manager" precedent (Filament) and "32-bit mask" precedent (Unity/Godot). | consume-now — Task 18's `RenderableDesc.layers: uint32_t = ~0u` already follows the Unity/Godot 32-bit convention, not Filament's 8-bit one. | Confirmed in-plan: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:358`. Full cross-engine semantics (AND-test, defaults) deepened in matrix-issue07. | See matrix-issue07 (owns this row's acceptance criteria). |
| Visibility toggle independent of destroy | Filament has no separate boolean; hiding an object is done by `layerMask` selecting it out of every camera's mask, or `RenderableManager::destroy()`. No dedicated "soft hide" API found in the header surface reviewed. | consume-now, via the existing `layers` field — no new API needed. | Confirmed by the same `layerMask` search above (no `setVisible`/`setEnabled` method exists on `RenderableManager`). | Test: `setLayers(handle, 0)` removes the renderable from every camera's visible/culled counters next `build()` call, without touching `destroy()`/handle validity. |
| Handle lifecycle / generational safety | Filament: `utils::Entity` is an id+generation pair minted by `EntityManager` (thread-safe, epoch-based recycling, `isAlive()` query) (`Entity.h`, `EntityManager.h:47-326`); each component manager keeps its OWN dense instance array indexed via the entity, not the raw entity slot — i.e. entity identity and component storage are already split. | consume-now. | **Verified in-repo, reuse candidate**: `rx::core::Handle<Tag>`/`HandlePool<Tag, T>` (`src/rx_core/include/rx_core/handle.h:1-75`) already implements exactly this generational-id idiom and already backs `BindlessHandle` (`rx_rhi_vk/bindless.h`) and `MaterialHandle`/`TextureHandle` (`rx_material/material_system.h:36-44,63-70`). Per CLAUDE.md's "don't reinvent" rule, `RenderableHandle`/`LightHandle` should be `rx::core::Handle<struct RenderableTag>`/`<struct LightTag>` — the *type*, not necessarily the *pool* (see next row). | Test: create N, destroy a subset, create more (slots reused, generation increments); a stale handle from before a slot's reuse fails every accessor loudly (throw/optional, never silent wrong-object access or UB) — mirrors the existing `MaterialHandle`/`BindlessHandle` test pattern (grep those test suites for the precedent test shape before writing a new one). |
| SoA storage vs the existing `HandlePool<Tag,T>` (AoS) | Filament's actual data layout is **AoS per Filament's own doc** ("Array-of-Structures... primitives array, material instances... prioritizes per-entity coherency", research-p4-scene.md:31-35) but each *component manager* is still its own flat table addressed by a dense instance index — the AoS/SoA choice is orthogonal to the entity/component split; D19 (seed 11) explicitly commits RendererX to **SoA** for cache-friendly batch iteration at 30k+ objects, diverging from Filament's per-entity-coherency choice deliberately (performance-first policy, batched culling over contiguous transform/AABB arrays in `DrawListBuilder`, D15). | consume-now, with a concrete implementation risk to flag. | **Conflict finding** (see Conflicts section): `rx::core::HandlePool<Tag, T>` (`handle.h:26-72`) stores the entire `T` value inside one `Slot` (AoS — `struct Slot { T value; uint32_t generation; bool alive; }`, one `std::vector<Slot>`). Reusing it wholesale for `RenderableManager`'s storage would force an AoS `RenderableDesc`-shaped row per slot, contradicting D19's SoA requirement and Filament's own "own dense table per component manager" pattern this ticket is supposed to follow. | Test: a synthetic-scene benchmark iterates `Scene`'s transform/AABB/layer arrays for 30k renderables and asserts the iteration is over contiguous `std::vector<T>` columns (one per field), not one `std::vector<RenderableDesc>` — inspectable via the SoA managers' own public accessors returning `std::span<const T>` per field, not per-object structs. |
| Transform hierarchy at runtime | Filament `TransformManager`: `create(entity, parent, localTransform)`, `setParent`, `getParent`, `getChildren`, world = compose(parent world, local) (TransformManager.h:171-277); local transforms are always parent-relative. | N/A-Phase-4 — **already ruled** by D12 ("glTF node trees are walked once at import; world transforms bake into per-instance transforms... Runtime hierarchy/re-parenting arrives with the animation phase"). Not re-litigated here; recorded for completeness since the issue asks what a HOST needs. | N/A. | Confirms: because Phase 4 has no parent composition, `Scene::setTransform(handle, mat4)` sets the **world** matrix directly (there is no "local" distinct from "world" when there is no parent) — this matches the issue's own consumer-boundary contract text (an ECS `RenderSyncSystem` already computed its own world matrices and hands them over verbatim). No additional API surface needed beyond the plan's existing `setTransform`. |
| Prev-frame transform slot (temporal cluster preserve-later) | Registry/D19 comment (2026-08-10, issue #5): "Lay the pool out so keeping a last-frame copy is a trivial later addition (e.g. double-buffered or copyable pool rows) — do not build the copy itself in Phase 4." Cross-referenced from `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:176-178` (post-processing's motion-vector requirement). | preserve-later (explicitly, binding per the issue comment). | N/A — layout-only requirement, no library involved. | Test: the transform SoA column's memory layout (e.g. `std::vector<glm::mat4>` per frame-slot index, or a single column with a documented stride) is demonstrated, in a unit test, to accept a second same-shaped array copied in without changing any existing accessor signature — i.e. add a stub "previous transforms" array in the test itself and assert it copies 1:1 with zero reshaping of the live column. |
| Dirty-tracking / update cost at 30k+ `setTransform` calls per frame | Brief asks what data layout Filament/Godot use for this. Filament's "O(1) local updates via transaction boundaries" (research-p4-scene.md:13) applies to **hierarchical** recomposition cost, which Phase 4 does not have (D12, flat). Godot's internal `RenderingServer` per-instance data layout was **not independently verified this session** (see Verification health) — the claim is not asserted as fact. | consume-now, with a stronger-than-precedent answer: no dirty-tracking machinery is *needed* in Phase 4 at all. | N/A. | Because D12 makes every proxy's stored transform already-flat (no parent to recompose against), `setTransform(handle, matrix)` is a single O(1) direct write into a dense SoA slot — there is no propagation pass to run before `DrawListBuilder::build()` reads it. Acceptance criterion: a benchmark (Tracy-zoned per CLAUDE.md's measured-claims policy) issues 30k `setTransform` calls and reports wall-clock time in the stage's published numbers; no "dirty bit" or two-pass update API should exist in the public surface (its absence is itself the criterion — if one is added, it needs its own justification). |
| LightManager forward-compatible sizing for point/spot | Filament `LightManager::Type` enum: `SUN, DIRECTIONAL, POINT, SPOT, FOCUSED_SPOT` (LightManager.h:199 area) — one manager, one storage shape, multiple types from day one. | preserve-later — **already ruled**, issue #5 comment (2026-08-11): "LightManager's storage admits future point/spot types structurally... even though only directional is consumed until the techniques phase" (FG2). Recorded for completeness, not re-litigated. | N/A. | Test: the internal light SoA reserves a `type` field/enum and params sized for punctual fields (position, range/falloff, cone angles) even though only the directional path populates/consumes them in Phase 4; a device-free test constructs a (currently unused) point-light row and asserts no truncation/reinterpretation of its fields. |
| Camera: reversed-inf-Z projection helpers | Filament: render projection matrix always uses **far = infinity** internally ("The far plane distance is always set internally to infinity for rendering", Camera.h:138) while keeping culling correctness via a **separate, finite** `projectionForCulling` matrix (Camera.h:159-162,292-309; `getCullingProjectionMatrix()`, Camera.h:383). Community-verified reversed-Z-infinite-far GLM formula (right-handed/left-handed variants; https://gist.github.com/pezcode/1609b61a1eedd207ec8c5acf6f94f53a, cross-checked against Reed's "Depth Precision Visualized"). | consume-now — matches D13 exactly (near=1/far=0, reversed, GREATER_OR_EQUAL). | Reversed-Z math itself needs no external library (GLM already vendored provides the primitives to hand-write the matrix; the gist is a community reference, not a dependency to adopt). | Unit test (device-free, per Task 18's own step list) asserts `Camera::proj()` maps world-space near→NDC-depth 1.0 and world-space far-approaching-infinity→NDC-depth→0.0, matching D13's stated mapping exactly. |
| Camera: separate finite culling-frustum matrix | Same Filament citation as above — the render projection's infinite far makes its own far-plane row degenerate for Gribb-Hartmann extraction (the far-plane coefficient becomes a constant / near-zero-normal plane, i.e. "always passes," under an infinite-far projection). Filament's answer is to keep a second, finite `projectionForCulling` matrix specifically so `Frustum::getNormalizedPlanes()` (Frustum.h:78-89, six real planes, "left, right, bottom, top, far, near" order) yields six *usable* planes. | consume-now — this is an **API-shape** consequence for `rx::scene::Camera` (edge-case handling itself is detailed in matrix-issue06). | UNVERIFIED against the current Task 18 interface text: the plan's `Camera` struct (plan:356) exposes only `proj()`/`viewProj()`, no second finite-far culling projection. | Acceptance criterion: `Camera` either (a) exposes a second method (e.g. `cullingProj()`, finite far, e.g. camera-far-radius or a configurable large-but-finite value) for `DrawListBuilder` to derive planes from, or (b) documents precisely which 5 (not 6) planes remain meaningful when extracting directly from the infinite-far `viewProj()` and asserts culling never relies on the degenerate one. Either is acceptable; silence (current state) is not — flagged as a Conflict below. |
| Camera: exposure ownership | Filament: `Camera::setExposure(aperture, shutterSpeed, sensitivity)` (photographic model) plus a convenience `setExposure(float)` overload (Camera.h:510-525); exposure interacts with light intensities (lux/lumens) to produce final scene brightness — owned by **Camera**, not the tonemap stage. | See Conflicts — D22 places "manual exposure parameter" on **the tonemap**, not Camera; Task 18's `Camera` struct (plan:356) has no exposure field at all. | N/A — no RendererX implementation exists yet. | Once resolved (coordinator adjudicates ownership): if Camera-owned, `Camera` gains an `exposure`/`setExposure()` API analogous to Filament's, consumed by the tonemap pass via a value, not a duplicated parameter; test asserts one source of truth (setting exposure on Camera changes rendered brightness without a second, independent tonemap-stage exposure knob able to silently disagree). |
| Camera: jitter hook (TAA preserve-later) | Filament places jitter on **`View::TemporalAntiAliasingOptions`** (`View.h:87,375,382`), not on `Camera` — RendererX has no `View` type distinct from `Camera` in the Task 18 interface, so Camera is the correct RendererX-specific analog by elimination, not a precedent match. | preserve-later (per issue #5's Camera-completeness bullet). | N/A. | Test: `Camera`'s projection-matrix computation path accepts an optional jitter offset (e.g. `glm::vec2 jitter = {0,0}` defaulted inert) threaded through to `proj()`'s translation terms, unused/zero in Phase 4, but present so the temporal cluster does not need to change `Camera`'s call signature later. |
| Frustum plane extraction — API placement | Gribb & Hartmann (cited above) is the standard method: extract 6 planes directly from the combined view-projection matrix's rows, no separate geometric computation. Filament's `Frustum::getNormalizedPlane(s)` (Frustum.h:71-89) is the precedent for exposing this as a first-class, camera-adjacent type rather than inlining it in the culling loop. | consume-now — placement question for Task 18 (edge-case correctness itself belongs to matrix-issue06). | N/A — math only, GLM-computable. | Task 18's own step list already commits to a unit test for "frustum plane extraction correctness" (plan:366); acceptance criterion: the extraction lives on `Camera` (or a small free function taking `Camera::viewProj()`) so `DrawListBuilder` (matrix-issue06) consumes already-extracted planes rather than re-deriving them per view per frame. |
| Per-submesh material override field | D14 text says "already planned"; Task 18's `RenderableDesc` comment says "per-submesh material overrides optional" (plan:358) but supplies no concrete typed field — Filament's equivalent, `setMaterialInstanceAt(instance, primitiveIndex, materialInstance)` (RenderableManager.h:28), is a real runtime-settable per-primitive slot, not a comment. | consume-now, but the plan text needs concretizing. | N/A. | Acceptance criterion: `RenderableDesc`/the create API takes an actual typed override list (e.g. `std::span<const std::optional<asset::MaterialHandle>>` indexed by submesh), not left as a doc comment; a unit test creates a multi-submesh renderable, overrides one submesh's material, and asserts `DrawListBuilder` emits the override (not the mesh's default) for that submesh's draw record only. |
| Renderable-level GPU instancing (author-declared, shared-bbox) | Filament `Builder::instances(instanceCount[, InstanceBuffer*])`, up to 32767 (or `Engine::getMaxAutomaticInstances()`, 64 typical), all instances share one bounding box (RenderableManager.h:577-614). | N/A-Phase-4, justified — distinct mechanism from D26.3's automatic post-sort instancing collapse; the collapse already delivers the throughput goal (identical-state runs merge automatically) without an author-facing API. Retrofit cost if wrong: low (additive opt-in API later, doesn't change the collapse path). | N/A. | No test needed while N/A. |
| Eviction-tolerant handle resolution (D24) | N/A — internal invariant, not an external precedent row. | consume-now — binding per D24, enforced at every handle-mediated resolve. | N/A. | `Scene::createRenderable`'s `asset::MeshHandle`/material-handle fields must resolve through the same residency-tolerant path D24 mandates for `DrawListBuilder` (matrix-issue06); a proxy referencing an evicted mesh/material must substitute the fallback asset (D11), never dereference a stale pointer. Test: create a renderable against a handle, force-evict the backing asset (test hook), assert `build()` still succeeds using the fallback, never a crash. |

---

## Conflicts

1. **Camera culling-frustum matrix — silent, not decided.** Task 18's
   `Camera` struct (plan:356) exposes only `proj()`/`viewProj()` (reversed,
   infinite far per D13). Filament's own documented reason for keeping a
   *separate* finite `projectionForCulling` matrix is precisely to avoid
   extracting a degenerate far plane from an infinite-far projection
   (Camera.h:138-162). D15 says culling uses "camera planes extracted from
   the (reversed-Z) view-proj" — the SAME matrix D13 defines as infinite-far
   — without addressing the degenerate-plane consequence. Quote, D13:
   "`rx::scene::Camera` owns the projection helpers so samples cannot get it
   inconsistently wrong." Quote, D15: "Frustum culling: camera planes
   extracted from the (reversed-Z) view-proj." Neither text resolves
   whether the far plane is dropped, treated as a no-op, or derived from a
   second finite matrix. Not resolved here per gate method; flagged for the
   coordinator.

2. **Exposure ownership — Camera (Filament precedent) vs tonemap (D22).**
   D22 text: "Manual exposure parameter on the tonemap (G-item; auto-exposure
   is techniques-phase)." Filament precedent: exposure lives on `Camera`
   (`setExposure`, Camera.h:510-525) and interacts with physically-lit light
   intensities. Task 18's `Camera` struct has no exposure field. If D19's
   own justification for following Filament ("no ECS, Filament-precedent
   managers... plus a plain Camera value type") is taken at face value,
   exposure arguably belongs on Camera; D22 places it downstream instead.
   Not resolved here; flagged for the coordinator.

3. **Priority/blend-order absence vs D26/D27's determinism requirement.**
   D26/D27 (via issue #6's amendment) require deterministic sort output
   "across thread counts" and a single canonical key. Filament's precedent
   treats `priority` and per-primitive `blendOrder` as load-bearing,
   first-class tiers of its sort key (`RenderPass.h:158-159` priority;
   `:100-104` blendOrder), not optional decoration — their absence from
   D14's key definition is a completeness gap, not merely a "nice to have"
   (see New gaps below), but is listed here too because it directly bears
   on determinism: two draws with identical (pipeline, material,
   depth-bucket) and no tie-break are order-ambiguous under `std::sort`
   unless the sort is stable AND some deterministic tie-break (e.g. handle
   index) already exists. D14 does not specify a final tie-break field.

## New gaps

- **Priority (coarse per-object draw-order control).** Present in Filament
  as a first-class 3-bit sort-key tier (see matrix row); absent from D14/D19
  and Task 18's `RenderableDesc` entirely. Proposed phase fit: Phase 4
  Stage 2 (Task 18/19), since adding it after `DrawPayload`'s layout ships
  (D26.2's packed SoA layout) means reshaping a structure D26 explicitly
  wants stable/indirect-compatible. Retrofit economics: expensive later
  (touches `RenderableDesc`, the SoA row, and the sort-key bit layout
  simultaneously) — this is exactly the "forces expensive changes at
  existing call sites later" test the gate's disposition rule asks about,
  so `N/A-Phase-4` would not be a defensible verdict for this one.
- **Per-primitive blend order tie-break.** Same phase-fit/retrofit argument
  as priority; see matrix row and Conflict #3.
- **Camera exposure API surface.** Not present anywhere in the plan text as
  a concrete field/method; see Conflict #2.

## Verification health

**Verified first-hand (fetched/read this session, cited by file:line or
URL):** Filament `RenderableManager.h`, `RenderableManager.cpp`,
`TransformManager.h`, `LightManager.h` (type enum only), `Camera.h`,
`Frustum.h`, `View.h` (jitter location only), `Entity.h`, `EntityManager.h`
(generation/isAlive), `EngineEnums.h` (channel count = 8), all at Filament
commit `d438927c1179b9abe7ede4efab7e2d4c2858d3e1`; in-repo `handle.h`,
`material_system.h`, `bindless.h`; the Task 18 plan text and D12/D13/D19/
D24/D26/D27 spec text; issue #5's two comments via `gh issue view --json`.
Godot's VisualInstance3D/Camera3D layer counts verified via a live web
search citing docs.godotengine.org (not a raw fetch of the doc page itself).

**Inferred, not independently verified:** Godot's *internal* `RenderingServer`
per-instance data layout (AoS vs SoA, dirty-tracking mechanics) — the brief
asked what data layout Filament/Godot use for 30k+-object update churn;
Filament's answer is verified (per-entity AoS at the component-manager
level, per research-p4-scene.md's own prior citation), Godot's is not. This
matrix does not assert a Godot internal-layout fact; it states only that the
question is unresolved for Godot specifically.

**Dead links / version ambiguity:** none encountered — all Filament fetches
resolved against a pinned commit SHA captured this session (not a floating
"main" reference). The reversed-Z-infinite-far GLM gist
(pezcode/1609b61a1eedd207ec8c5acf6f94f53a) is a community reference, not an
official library or a RendererX dependency candidate — cited only to
cross-check the matrix formula's shape, not as a "library support" claim.
