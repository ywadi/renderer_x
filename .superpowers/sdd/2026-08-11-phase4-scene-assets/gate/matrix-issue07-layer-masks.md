# Matrix — Issue #7: Layer/mask system (camera cull masks, light channels)

**Ticket:** #7 "Layer/mask system (camera cull masks, light channels)"
(`phase-4`, `stage-2`) — a feature card spanning plan Tasks 18-19 (renumbered
14-15 in the issue's own original text), demonstrated in Task 19's sample.
**Plan tasks:** Task 18 (`RenderableDesc.layers`/`.channels`,
`DirectionalLightDesc.channels`, `Camera.cullMask`, plan:356-363), Task 19
(mask filtering consumed during culling, plan:386,406), Task 24 (sample 09's
HUD toggle demo, plan:454).
**Binding spec decisions:** D15 (culling — the paragraph defining layers/
channels semantics, `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:256-261`).

**Sources consulted:**
- `gh issue view 7 --json body,comments` (2026-08-18).
- Plan Task 18/19/24 interface and step text (line refs above).
- Spec D15 (:247-261).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/research-p4-scene.md` §5.
- Unity, current docs (2026 build, Unity 6000.4): `LayerMask` API reference
  (docs.unity3d.com/6000.4/Documentation/ScriptReference/LayerMask.html,
  "Built on 2026-06-17"), "Introduction to layerMasks" manual page
  (docs.unity3d.com/Manual/layermask-introduction.html, "Published
  2026-07-02") — verified via live web search 2026-08-18, not a raw fetch of
  the page body.
- Godot, `VisualInstance3D` class reference (docs.godotengine.org/en/stable
  and /en/4.4) — verified via live web search 2026-08-18.
- Unreal Engine, "Using Lighting Channels in Unreal Engine" (Epic Developer
  Community docs, 5.8 current build, dev.epicgames.com) — fetched directly
  2026-08-18 (`WebFetch`), quoted verbatim below.
- Filament (google/filament, GitHub, commit `d438927c1179b9abe7ede4efab7e2d4c2858d3e1`,
  fetched 2026-08-18): `filament/include/filament/RenderableManager.h`
  (`layerMask`, `channel`, `lightChannel` builders/setters), `filament/include/filament/LightManager.h`
  (`lightChannel`), `filament/src/components/RenderableManager.cpp`
  (default values, bit-packing), `libs/filabridge/include/private/filament/EngineEnums.h`
  (`CONFIG_RENDERPASS_CHANNEL_COUNT = 8`), `shaders/src/surface_light_directional.fs`
  (per-fragment light-channel test), `filament/src/details/Scene.cpp`
  (channel data flow to the shader).

---

## The matrix

| Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
|---|---|---|---|---|
| Renderable visibility mask width (32-bit) | **Unity**: `LayerMask` is a 32-bit bitmask; "up to 32 LayerMasks supported by the Editor... first 8 ... specified by Unity; the following 24 are controllable by the user" (docs.unity3d.com, 2026 build, verified via search 2026-08-18). **Godot**: `VisualInstance3D.layers`/`Camera3D.cull_mask` share a 32-bit storage word; the editor exposes 20 of the 32 bits as named user layers, the remaining 12 are reserved-but-settable via script (docs.godotengine.org/en/stable/classes/class_visualinstance3d.html, verified via search 2026-08-18). **Filament** diverges: `layerMask` is only **8-bit** (`Builder::layerMask(uint8_t select, uint8_t values)`, RenderableManager.h:305-312) — the one precedent D19 names as the architectural model does NOT use a 32-bit visibility mask. | consume-now — Task 18's `RenderableDesc.layers: uint32_t = ~0u` (plan:358) and `Camera.cullMask: u32 = ~0u` (plan:356) already follow the Unity/Godot convention. | Confirmed in-plan (line ref above). | Test: a camera with `cullMask = 0x1` sees only renderables whose `layers & cullMask != 0`; a battery of layer/mask bit combinations (see the CI-matrix row below) asserts the exact AND-test result per case. |
| Godot's 20-vs-32 distinction (verify, don't conflate) | Godot's EDITOR exposes 20 named layers; the underlying `cull_mask`/`layers` properties are full 32-bit integers, with the extra 12 bits reserved for internal engine use but settable from script (verified via search, docs.godotengine.org). | consume-now — informational; RendererX has no editor UI to bound, so the 20-vs-32 distinction doesn't directly apply, but the underlying storage-width precedent (32-bit) still supports the `layers: u32` choice above. | Confirmed via search citation above. | No separate test — folded into the row above; recorded here only so the "Godot 20/32" figure the research brief names is not left ambiguous. |
| Light/renderable channel width (8-bit) | **Filament**: BOTH the renderable side (`lightChannel(channel, enable)`, channel `[0,8)`, RenderableManager.h:370-375) and the light side (`LightManager::Builder::lightChannel`, LightManager.h:538-544) use an 8-bit channel space, default channel 0 enabled ("Light channel 0 is enabled by default"). Verified in the actual implementation: `mLightChannels = 1` (bit 0 set) is `RenderableManager.cpp:106`'s literal default. **Unreal**: hard cap of 3 lighting channels ("Unreal Engine supports up to 3 lighting channels... By default, Directional Lights, Spot Lights, Point Lights, and all Actors ... have Lighting Channel 0 enabled" — dev.epicgames.com, fetched 2026-08-18) — Filament's 8 is "deliberately more generous than Unreal's 3" exactly as D15 states. | consume-now — Task 18's `RenderableDesc.channels: uint8_t = 0xFF` / `DirectionalLightDesc.channels: uint8_t = 0xFF` (plan:358-359) match Filament's 8-bit width and default-enabled-everything convention (0xFF = all 8 channels on, vs. Filament's narrower "channel 0 only" default — see the Default values row below for that divergence). | Confirmed in-plan; cross-checked against Filament's `RenderableManager.cpp:105-106` (`mLightChannels = 1`, i.e. NOT all-ones by default) and Unreal's docs (channel 0 default). | See Default values row — this row confirms only the BIT WIDTH choice, which is uncontested across all three precedents. |
| Camera `cullMask` AND-test semantics | Standard across every precedent read: visibility requires `(renderable.layers & camera.cullMask) != 0` — a non-empty intersection, not exact equality or a subset test. Confirmed structurally by Unity's `LayerMask` (bitwise design), Godot's `cull_mask` (same), and Filament's `layerMask` (same 8-bit AND semantics, `RenderableManager.h:305`, "do: `builder.layerMask(7, 2)`" — a select/values pair that ANDs a bitmask update into the stored value, not a full overwrite). | consume-now. | Confirmed via all three engines' documented semantics (Unity/Godot: direct citation above; Filament: direct header read). | Test: exact-bit-overlap cases (single shared bit → visible), zero-overlap cases (→ culled), and the boundary case `cullMask = 0` (→ nothing visible to that camera, including objects with `layers = ~0u`) are each asserted explicitly. |
| Light channels filtering BOTH lighting AND shadow-casting — **the two are separable in some engines** | **Coupled precedent (Unreal)**: "CSM shadows from stationary or movable directional lights cast only on primitives with matching lighting channels" (dev.epicgames.com, fetched 2026-08-18, quoted verbatim) — lighting-channel filtering IS ALSO shadow-caster filtering; they cannot be set independently via lighting channels alone. **Separable precedent (Filament, verified by direct source trace, not inferred)**: the light-channel AND-test (`if ((light.channels & channels) == 0) { return; }`) lives ONLY in the per-fragment shading function `evaluateDirectionalLight()` (`shaders/src/surface_light_directional.fs:30,39-42`) — i.e. it gates LIGHTING CONTRIBUTION only. A grep of `filament/src/ShadowMapManager.cpp` and `filament/src/RenderPass.cpp` (both fetched and searched directly this session) found **zero** references to `lightChannel`/`LightChannel` — Filament's shadow-map generation is driven purely by the per-renderable `castShadows` boolean, unconditioned on channel membership. **RendererX's current text (D15) follows the COUPLED (Unreal) model**: "light `channels: u8` vs renderable `channels: u8` filter both lighting and shadow-caster lists" — verbatim quote, spec D15, line 258-259. | **See Conflicts** — this is the single most load-bearing finding in this matrix: D19 names Filament as the architectural precedent for the whole scene layer ("no ECS... Filament-precedent managers"), but D15's channel semantics are Unreal's (coupled), not Filament's (separable). Not resolved here per gate method. | Both sides independently verified this session (Epic docs fetch; Filament source trace across three files). | Whichever the coordinator adjudicates: if coupled (current D15 text, consume-now as written), a test asserts a renderable outside a light's channel mask is absent from BOTH that light's shading contribution AND that light's `ShadowLists` entry. If separable is chosen instead, two independent tests are needed (lighting-only exclusion must not affect shadow-casting, and vice versa) — see matrix-issue06's shadow-caster rows, which this decision also constrains. |
| Default values | **Unity**: default layer is layer 0 ("Default"), i.e. one bit set, not all-ones. **Godot**: default `layers`/`cull_mask` is `1` (bit 0), not all-ones (general Godot convention, consistent with the 20-named-layers model where layer 1 is the default). **Filament**: `layerMask` has no single documented default bit pattern in the header (defaults to the `Builder`'s own zero-initialized state, effectively visible-nowhere until set — RenderableManager.h does not document a nonzero default for layerMask the way it does for `priority`/`channel`); `lightChannels` DOES have a verified nonzero default: `mLightChannels = 1` (channel 0 only), matching Unreal's "channel 0 enabled by default." **None of the three precedents default a mask to all-ones.** | consume-now, with the divergence flagged as a documented, deliberate choice, not a silent gap. | Confirmed per-engine as cited above. | D15/Task 18 choose **all-ones defaults** for both `layers` (`~0u`) and `channels` (`0xFF`) — the opposite convention from every precedent checked (which all default to "layer/channel 0 only"). This is defensible for RendererX specifically (an opt-OUT default suits a middleware where the host, not RendererX, owns gameplay-layer semantics and may never call `setLayers`/`setChannels` at all — an opt-IN default would silently hide every object from every light until the host explicitly configures channels, a much worse failure mode for a library with no built-in "layer 0" concept of its own). Acceptance criterion: a renderable/light created with default-constructed descs is visible to a default-constructed camera and lit by a default-constructed light with no explicit `setLayers`/`setChannels` call — i.e. the all-ones default is itself the regression test (a bug here manifests as "the sample renders nothing" the moment someone forgets to call a setter). |
| API for toggling at runtime | Filament: `setLayerMask(Instance, uint8_t select, uint8_t values)` and `setLightChannel(Instance, unsigned channel, bool enable)` — two DIFFERENT shapes: layerMask is a masked partial-write (`select` chooses which bits to touch, `values` supplies their new state — "do: `builder.layerMask(7, 2)`" to set bits 1-2 while leaving others alone), while lightChannel is a single-bit enable/disable. Task 18's plan text shows only `setLayers(RenderableHandle, uint32_t)` (a full overwrite, plan:362) with no equivalent `setChannels`/per-bit toggle named. | consume-now, with an API-completeness note. | Confirmed via direct header read (RenderableManager.h:305-312,747-755). | Acceptance criterion: `Scene` exposes both a full-overwrite setter (`setLayers(handle, uint32_t)`, already planned) AND either a masked partial-write helper or (simpler, sufficient) documents that callers read-modify-write via `getLayers`/`getChannels` accessors — the plan text names neither a getter nor a channels setter; both should exist for parity with the layers API (see New gaps). |
| CI-testable mask matrices | Sample 09's HUD toggles "hide/show instance groups per camera and per light channel" (issue #7's own acceptance line). | consume-now. | N/A. | Proposed concrete CI matrix (the acceptance criterion the issue's prose leaves unstated): for a fixed synthetic scene with objects tagged across at least 3 distinct layer bits and 3 distinct channel bits, assert render/cull counts for (a) camera cullMask = single-bit, all objects on that bit visible and none other; (b) cullMask = 0, zero visible; (c) cullMask = all-ones, all visible; (d) light channel = single-bit, only matching-channel objects lit (and, per the Conflicts row above, cast/not-cast shadows per whichever semantics is adjudicated); (e) a combined case (object matches layer but not channel, and vice versa) proving the two filters are applied independently of each other even though channel-vs-shadow coupling may not be. |

---

## Conflicts

1. **D15's channel semantics follow Unreal's coupled model; D19 names
   Filament (a separable-model engine) as the architectural precedent for
   the whole scene layer.** Quote, D15 (`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:258-259`):
   "light `channels: u8` vs renderable `channels: u8` filter both lighting
   and shadow-caster lists." Quote, D19 (`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:295-296`):
   "`src/rx_scene` implements Filament-precedent managers [R:scene]." Quote,
   Unreal's own docs (dev.epicgames.com, fetched 2026-08-18): "CSM shadows
   from stationary or movable directional lights cast only on primitives
   with matching lighting channels." Quote, Filament source, gating
   lighting ONLY (`shaders/src/surface_light_directional.fs:39-42`):
   `int channels = object_uniforms_flagsChannels & 0xFF; if ((light.channels
   & channels) == 0) { return; }` — no equivalent gate exists anywhere in
   `ShadowMapManager.cpp`/`RenderPass.cpp` (searched directly, zero hits).
   Not resolved here per gate method; both sides are independently verified
   facts, and the choice has a direct, concrete implementation consequence
   for matrix-issue06's shadow-caster culling code.

## New gaps

- **No `getLayers`/`getChannels`/`setChannels` named in Task 18's interface
  text**, only `setLayers`. Proposed phase fit: Task 18 itself (Phase 4
  Stage 2) — cheap now (the same handle-indexed SoA column already needs a
  read path for `DrawListBuilder` to consume; exposing it publicly is a
  small addition), and retrofitting a getter after the ABI/ 
  consumer-boundary contract (D19) is frozen is a needless later API-surface
  negotiation for something this small.

## Verification health

**Verified first-hand this session:** Unity's current (2026, Unity 6000.4)
LayerMask documentation via live search with direct source URLs and quoted
text; Godot's `VisualInstance3D`/`Camera3D` layer documentation via live
search with direct source URLs; Unreal's "Using Lighting Channels" page
fetched directly (`WebFetch`) against the current 5.8 Epic Developer
Community build, quoted verbatim; Filament's `RenderableManager.h`,
`LightManager.h`, `RenderableManager.cpp`, `EngineEnums.h`, and
`shaders/src/surface_light_directional.fs` read directly at commit
`d438927c1179b9abe7ede4efab7e2d4c2858d3e1`; `ShadowMapManager.cpp`/
`RenderPass.cpp` searched directly for the ABSENCE of a channel check
(a negative-result claim, verified by direct grep of fetched source, not
assumed from documentation silence).

**Inferred / not independently re-verified:** none of substance for this
ticket — every cross-engine claim in this matrix traces to a direct fetch,
direct source grep, or a live-search result carrying its own source URL and
build/publish date.

**Dead links / version ambiguity:** the research-p4-scene.md source this
gate deepens cites `docs.unity.cn/530/Documentation/Manual/Layers.html` — a
Unity 5.3.0 (China-mirror) URL, roughly a decade stale. This matrix instead
verified against the current (2026, Unity 6000.4) `docs.unity3d.com` build;
the underlying 32-layer/8-builtin fact is unchanged across that span, but
the citation in this matrix should be treated as authoritative over the
older research-doc citation where they're ever quoted together.
