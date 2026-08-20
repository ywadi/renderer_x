# Completeness matrix — P5 T18 (issue #54): Spot shadow atlas + point shadows

**Plan task:** Task 18, Stage 2 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:573-591`).
Depends on T16 (`T18, T19 after T16`, plan:987). Charter ladder:
*"spot = shadow atlas; point = cubemap or dual-paraboloid atlas"*
(design doc:430-431).

**Sources consulted (in-repo, 2026-08-20):** `src/rx_shadow/include/
rx_shadow/shadow_caster_pipeline.h` (the depth-only pipeline this ticket's
per-light-type render passes reuse — built OUTSIDE MaterialSystem, D28/RC3,
already parameterized on `depthFormat`); T14's own froxel-based
per-light-shadow-INDEX integration point (matrix-p5t14/15's own
`RxDrawData` extension pattern — this ticket's atlas slot index is a
DIRECT analogue of T15's froxel-buffer index, same "add a bindless index
field to the per-draw/per-light uniform row" idiom).

**Sources consulted (external, fetched 2026-08-20, pinned commit
`721ec800093de984cbee155e459298b6b2dbb855`, `google/filament`):**
`filament/src/ShadowMapManager.cpp:1247-1332` (`cullPointShadowMap` —
cubemap-face view/projection construction, read in full),
`ShadowMapManager.cpp:1334-1420` (`calculateTextureRequirements` — the
REAL atlas allocator vs. simple-array-layer fallback, both paths read
directly).

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Spot shadow atlas — real packing allocator, not one-layer-per-light | Filament ships BOTH a real `AtlasAllocator` (variable-size slot packing into one texture, `calculateTextureRequirements()`'s `allocateFromAtlas` closure, ShadowMapManager.cpp:1355-1365) AND a simpler `allocateFromTextureArray` fallback (one fixed-size array LAYER per shadow map, no packing, :1367-1374) — BOTH exist in the same file, selected by a feature flag (`mFeatureShadowAllocator`, :1376-1377). The charter's OWN language ("shadow lights render into a shadow ATLAS," ticket #54 body) names the atlas variant specifically, not the array fallback. | consume-now (atlas variant) | VERIFIED via direct source read of both closures — the atlas path is real, production Filament code (not a documented-but-unused feature), and the charter's own wording ("allocator over one atlas texture; slot policy per spec") matches it precisely over the simpler array alternative. | Acceptance criterion: N spot lights of VARYING requested resolution (not all identical) pack into ONE atlas texture via a real allocator (2D bin-packing or a fixed-grid-of-tiles scheme — RendererX's own choice of allocator algorithm, Filament's own `AtlasAllocator` internals not independently re-fetched this session, see Verification health) — a unit test asserts no two lights' allocated viewports overlap, and an allocator EXHAUSTION test (the plan's own named "atlas exhaustion past declared capacity" criterion, plan:588-589) asserts a request that cannot fit fails LOUDLY (a defined error/fallback-light-drop path) rather than silently corrupting an existing allocation. |
| N spots share one atlas with correct per-light lookups | Plan's own named acceptance criterion (plan:584-585): *"N spots share one atlas with correct per-light lookups (readback probes per light; two lights swapped → probes discriminate)."* | consume-now | N/A — a correctness-test methodology requirement; mirrors matrix-p5t14's own "exact membership" discrimination-test discipline (never certify "an atlas exists," certify "THIS light's shadow reads from THIS light's own slot, not a neighbor's"). | GPU test (concretizing the plan's own sketch): two spot lights, each with a DISTINCT, known occluder/shadow pattern, share one atlas; swap which light illuminates which test scene (or swap their atlas slot assignment) and assert the rendered shadow pattern SWAPS correspondingly — proving per-light atlas-slot addressing is correct, not merely "an atlas was allocated." |
| Point-light shadows: cubemap (6 faces), not dual-paraboloid | Filament's OWN implementation choice, verified directly: `cullPointShadowMap()` (ShadowMapManager.cpp:1247-1290) explicitly constructs a PER-FACE view matrix (`ShadowMap::getPointLightViewMatrix(TextureCubemapFace(face), position)`) and a 90°-FOV perspective projection (`mat4f::perspective(90.0f, 1.0f, 0.01f, radius)`, :1262) — SIX such faces per point light, laid into the SAME atlas/array infrastructure as spot lights, "guaranteed to be sequential" so the shader can find sibling faces from the first face's index (:1308-1309 comment). No dual-paraboloid code path exists anywhere in the fetched `ShadowMapManager.cpp`/`ShadowMap.cpp`. | consume-now (cubemap) | VERIFIED — this is a real, exercised precedent, not a documented-only option; the charter's own "cubemap or dual-paraboloid" phrasing (design doc:430-431) presents both as EQUALLY valid, but Filament's own actual, current, production choice is unambiguously cubemap. Dual-paraboloid is the historically bandwidth-saving alternative for constrained (older mobile) hardware — not what the project's own named reference source (Filament) actually ships. | See Open Questions #1 — this row states the finding; the ticket-level ruling is recorded there. |
| Point-light near/far-plane convention | Filament, quoted exactly: `mat4f::perspective(90.0f, 1.0f, 0.01f, radius)` (ShadowMapManager.cpp:1262) — near HARDCODED to 0.01 (world units), far = the light's OWN attenuation-falloff radius (`POSITION_RADIUS.w`, read at :1258), not the scene's own camera far plane. | consume-now | VERIFIED via direct source read — a genuinely useful, easy-to-miss convention: a point light's shadow-map far plane should track ITS OWN physical falloff radius (T13's own range-window field, matrix-p5t13's own `LightRecord::range`), not any camera/scene-wide constant — a light with a smaller `range` gets a tighter-fitted (higher-precision) shadow frustum "for free." | Unit test: two point lights with DIFFERENT `range` values produce DIFFERENT per-face projection matrices whose far plane exactly equals each light's own range — proving the coupling is live, not a shared/hardcoded constant. |
| Cross-face seam continuity | Plan's own named acceptance criterion (plan:586-587): *"Point-light shadows continuous across face/hemisphere seams (seam probe at a boundary direction)."* | consume-now | N/A — a correctness-test methodology; the classic cubemap-shadow failure mode is a visible seam/discontinuity exactly AT a face boundary (e.g. the +X/+Y edge) if the two adjacent faces' depth comparisons are not consistently biased/filtered across the boundary — a known, well-precedented artifact class for cubemap shadows generally (not specific to Filament's own code, general cubemap-rendering knowledge). | GPU test: a caster positioned so its shadow spans exactly across a known cubemap face boundary (e.g. a receiver plane straddling the +X/+Y edge direction from the light) shows a CONTINUOUS shadow silhouette across the seam — no visible depth/bias discontinuity — assert via a probe pair sampled on either side of the boundary within a small angular tolerance. |
| Shadowed spot/point integrate with clustered (froxel) lists via per-light shadow indices | Ticket's own text (#54 body): *"Shadowed spot/point integrate with the clustered lists (per-light shadow indices)."* Filament's own `shadowInfo[lightIndex].index` (ShadowMapManager.cpp:1312) is the EXACT precedent: a per-light shadow-atlas-slot INDEX stored alongside the light's other per-light data, read by the shader from the SAME light record the froxel/cluster path already indexes into (`surface_light_punctual.fs`'s own `light.shadowIndex`, cited in matrix-p5t15's own research). | consume-now | Cross-references matrix-p5t15's own `RxDrawData`/light-record extension pattern directly — this is the SAME mechanism (a bindless atlas-slot index riding along in the light's own per-instance data T14/T15 already establish), not a new one. | Acceptance criterion: T15's own per-light data record (whatever shape T14/T15 land, per matrix-p5t14/15's own open questions) gains a `shadowAtlasSlotIndex` (or per-cubemap-face base index) field the froxel-lit fragment shader reads to select which atlas region to sample — a GPU test confirms swapping two lights' atlas assignments (same test shape as the "N spots" row above) still resolves correctly through the FULL clustered-lighting path, not just a standalone shadow-only test. |
| Atlas exhaustion past declared capacity — loud, defined, never corrupt | Plan's own explicit content-scale rule (plan:588-589) + the standing CLAUDE.md content-scale-testing rule (plan:82-87). | consume-now | N/A — standing rule; concretized against whatever capacity the atlas allocator (row above) declares. | GPU/unit test: request one MORE shadowed light than the atlas can hold (all requested at the SAME resolution tier, to make the capacity boundary unambiguous) — the (capacity+1)-th light's shadow request fails LOUDLY (a counter/log, and a defined fallback: e.g. that light renders UNSHADOWED rather than corrupting another light's already-allocated slot) — never a silent overwrite of an existing allocation. |

---

## Open Questions

1. **Cubemap vs. dual-paraboloid for point lights — the charter presents
   both as valid; this gate's research finds Filament's own actual,
   current, production choice is unambiguously cubemap (see matrix row
   above).** **Recommendation: cubemap.** Beyond matching the named
   port-source's real behavior (not just a documented option), cubemap
   shadows integrate CLEANLY with the atlas-allocator infrastructure this
   SAME ticket already needs for spot lights (six atlas slots per point
   light, sequentially laid out, exactly Filament's own approach) — no
   SEPARATE rendering/sampling code path is needed the way dual-paraboloid
   would require (a distinct warp-projection vertex-shader stage and a
   distinct seam-handling discipline at the paraboloid's own equator, a
   historically trickier artifact class than cubemap's simple face
   boundaries). Dual-paraboloid's traditional advantage (fewer draw calls
   — 2 renders instead of 6) is a real Deck-floor cost consideration, but
   Filament's own choice suggests it is not the dominant one in a modern
   engine's actual practice, and the six-cubemap-face draws reuse the
   EXACT SAME depth-only `ShadowCasterPipeline` (already delivered,
   `shadow_caster_pipeline.h`) six times with different view/proj
   matrices — genuinely low marginal implementation cost given what
   already exists.

## Verification health

**Verified first-hand this session:** `ShadowMapManager.cpp`'s
`cullPointShadowMap()` and `calculateTextureRequirements()` (including
both the atlas-allocator and simple-array-layer closures) were fetched and
read directly from `google/filament` at commit
`721ec800093de984cbee155e459298b6b2dbb855` — the cubemap-vs-dual-
paraboloid finding and the near/far-plane convention are both DIRECT
quotes from this read, not inferred or search-digested. In-repo
`shadow_caster_pipeline.h` re-confirmed against the working tree.

**Not independently re-verified:** `AtlasAllocator`'s own internal packing
ALGORITHM (`filament/src/AtlasAllocator.h`, named/imported at
ShadowMapManager.cpp:19 but not itself fetched this session) — this
matrix's atlas-packing acceptance criteria are written algorithm-agnostic
(no-overlap + loud-exhaustion) specifically so they do not depend on
knowing Filament's own exact bin-packing scheme; a follow-up fetch would
be needed only if the coordinator wants to port Filament's OWN packing
algorithm specifically rather than choosing an equivalent one
independently.
