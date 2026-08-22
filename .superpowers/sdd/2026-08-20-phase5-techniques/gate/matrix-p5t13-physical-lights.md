# Completeness matrix — P5 T13 (issue #49): Physical light units + punctual lights (KHR_lights_punctual consumption)

**Erratum (T15 review, 2026-08-22):** this matrix cites Filament commit
`721ec800093de984cbee155e459298b6b2dbb855` (lines 29, 89) as the source for
`LightManager.cpp`/`surface_light_punctual.fs` — that commit is `main`'s
same-day HEAD from Stage 2's research survey, not the `v1.75.0` release tag.
Per `rulings-2026-08-20.md` RC1 (the binding pin) and independently
confirmed via `git ls-remote https://github.com/google/filament
refs/tags/v1.75.0`, the TRUE `v1.75.0` tag commit is
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`. The cited line ranges were not
re-verified against that commit as part of this erratum; a future consumer
of this matrix's Filament citations should re-diff `721ec80...0e58877c0`
before relying on exact line numbers.

**Plan task:** Task 13, Stage 2 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:465-486`).
**Charter binding:** priority (3) "physical light units + clustered Forward+"
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:414-421,452-454`):
*"Physical light intensities/units... Punctual-light import consumption
(KHR_lights_punctual, preserved since Phase 4) turns on here."*

**Sources consulted (in-repo, read in full/relevant sections, 2026-08-20):**
`src/rx_scene/include/rx_scene/scene.h:150-238` (LightRecord/LightType/
DirectionalLightDesc — `Point`/`Spot` admitted in storage, zero public
creation surface); `src/rx_scene/scene.cpp:283` (`createDirectionalLight`,
the ONLY light-creation entry point that exists); `src/rx_asset/include/
rx_asset/mesh_asset.h:250-264,301-306` (`LightData`/`ImportedScene::lights`);
`src/rx_asset/import_gltf.cpp:1422-1450,1701,1839` (KHR_lights_punctual
parse into `LightData` — type/color/intensity/range/cone, verbatim glTF
values, no unit conversion applied); grep sweep (2026-08-20) confirms
`ImportedScene::lights`/`result.scene.lights` is populated but **never
consumed** anywhere in `src/` or `samples/` — no call site converts an
imported `LightData` into a `Scene` light of any kind. Sample 09's own
light (`samples/09_scene/main.cpp:1593-1594,1681`) is hand-authored, not
import-derived.

**Sources consulted (external, fetched 2026-08-20, pinned):**
- Khronos glTF `KHR_lights_punctual` spec, commit
  `2b29723d025a995971726f2989697cdc49b1222a`
  (`extensions/2.0/Khronos/KHR_lights_punctual/README.md`, fetched and
  quoted verbatim below) — the conformance ground truth.
- Google Filament, commit `721ec800093de984cbee155e459298b6b2dbb855`:
  `filament/src/components/LightManager.cpp:121-122,348-397` (unit
  conversion formulas, fetched/quoted below) and `shaders/src/
  surface_light_punctual.fs:93-114` (runtime falloff/cone-attenuation
  GLSL, fetched/quoted below).

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Directional light unit (lux) | KHR spec, quoted: *"Directional light intensity is defined in lumens per metre squared, or lux (lm/m²)... it is not attenuated."* | consume-now | **Already delivered**: `LightRecord::colorLux`/`DirectionalLightDesc::colorLux` (scene.h:211,224-226) is already named and typed in lux — Task 13 does not need to add this field, only wire import consumption and the direct-lighting shader math to actually treat it as lux (radiometric-to-shading conversion, see next row). | Unit test: a directional light created with `colorLux = {1,1,1}` and a known glTF-imported `intensity` value produces the SAME numeric `colorLux` the source file's `KHR_lights_punctual.lights[i].intensity` states — a straight pass-through, not a conversion (see "no conversion at import" row below). |
| Point/spot lights have NO public Scene creation API | — (in-repo gap enumeration). | **new work, in-scope** | VERIFIED: `Scene`'s only public light-creation method is `createDirectionalLight(DirectionalLightDesc)` (scene.h:369). `LightRecord`'s storage already carries `position`/`range`/`innerConeAngle`/`outerConeAngle` fields "structurally admitted... even though only directional is consumed until the techniques phase" (scene.h:189-193, quoting the FG2 comment this ticket itself closes) — the storage-layer work is done; the public surface is not. | Acceptance criterion: `Scene` gains `createPointLight(PointLightDesc)`/`createSpotLight(SpotLightDesc)` (mirroring `DirectionalLightDesc`'s own shape: position/color-in-candela/range/[cone angles for spot]/castsShadows/channels), each producing a `LightRecord` with the correct `LightType` — a unit test constructs one of each and asserts every field round-trips through `lightRecordForTesting()` unchanged. |
| glTF `intensity` units are ALREADY candela/lux — no lumen→candela conversion at import | KHR spec, quoted: *"`point` and `spot` lights use luminous intensity in candela (lm/sr) while `directional` lights use illuminance in lux... The `intensity` represents the luminous intensity that the light would emit if it were colored pure white."* | consume-now | The importer already stores the raw glTF `intensity` float verbatim into `LightData::intensity` (import_gltf.cpp:1429, no scaling/conversion applied) — CORRECT per the spec's own unit definition, since the glTF number IS already in the target physical unit. This is a genuinely easy point to get wrong: Filament's OWN `LightManager::setIntensity()` (LightManager.cpp:348-397) accepts an `IntensityUnit` enum (`LUMEN_LUX` vs `CANDELA`) and CONVERTS lumens→candela for its ARTIST-FACING authoring API — that conversion path is for Filament's OWN scene-authoring surface (e.g. "100W bulb at 100% efficiency"), NOT for glTF import, where the number in the file is already candela/lux by spec. | Import-consumption test (decoded-value discipline, per the phase's own standing rule): a fixture glTF with a `point` light `intensity: 1500` (candela) imports into a `Scene::createPointLight` call whose `LightRecord::colorLux`-equivalent intensity field is EXACTLY `1500.0`, not `1500 / (4π)` or any other lumen-style rescale — proving the importer does NOT apply the LUMEN_LUX→CANDELA conversion that would be correct for a *different*, lumens-denominated authoring surface. |
| RendererX's OWN synthetic/authoring light-creation surface MAY want a lumens/watts convenience API | Filament `LightManager::Builder::intensity(watts, efficiency)` (LightManager.cpp:121-122): `mIntensity = efficiency * 683.0f * watts` (683 lm/W is the luminous-efficacy-of-monochromatic-555nm-light constant, the standard photometric conversion Filament cites); `setIntensity(intensity, IntensityUnit)` (LightManager.cpp:348-397, quoted formulas below). | log-don't-drop | Point: `li = lp / (4π)` (LightManager.cpp:363-364, `luminousIntensity = luminousPower * ONE_OVER_PI * 0.25`). Spot (Filament's plain, non-"focused" `SPOT` type): `li = lp / π` (LightManager.cpp:387-390). Spot (Filament's `FOCUSED_SPOT`, intensity defined AT the cone rather than as total flux): `li = lp / (2π(1-cosOuter))` (LightManager.cpp:372-377) — the CORRECT physically-derived solid-angle formula for a cone of half-angle `outerConeAngle` (solid angle `Ω = 2π(1-cosθ)`), and the formula RendererX's own unit tests should match if it ever exposes a lumens-authoring path, since it is dimensionally exact (unlike the plain-`SPOT` `1/π` shortcut, which is Filament's own simplified/non-cone-aware variant). | Not required for Task 13's own import-consumption scope (glTF's own numbers are already in the target unit — see row above) — this row exists so a FUTURE lumens/watts-denominated authoring convenience (e.g. `PointLightDesc::fromLumens(lp)`) is built against the physically-correct cone-solid-angle formula, cited here, rather than re-derived incorrectly later. If Task 13 builds no such convenience API at all, this is a clean, documented registry deferral — not a Task 13 gap. |
| Point-light attenuation: inverse-square + range windowing | KHR spec, quoted VERBATIM: *"attenuation = max( min( 1.0 - (current_distance / range)⁴, 1 ), 0 ) / current_distance²"* (README.md:104-106) — the spec's own "recommended implementation," and the literal conformance target per this gate's own charter ("KHR spec exactness is a conformance criterion"). | consume-now | **A verified, load-bearing discrepancy with Filament's own shader**, not a stylistic footnote: `getSquareFalloffAttenuation()` (surface_light_punctual.fs:93-99) computes `factor = d²·falloff` (`falloff = 1/range²`, so `factor = (d/range)²`), then `smoothFactor = saturate(1 - factor²) = saturate(1-(d/range)⁴)` — matching the KHR window TERM exactly so far — but then **squares it again**: `return smoothFactor * smoothFactor`, i.e. Filament's windowed numerator is `(1-(d/range)⁴)²`, not the KHR spec's un-squared `(1-(d/range)⁴)`. The two formulas are numerically IDENTICAL only in the near-field (d≪range, where both →1) and diverge measurably as d→range (Filament's window falls off faster/sharper near the cutoff). See Open Questions — this is a genuine choice, not a bug in either source. | Analytic falloff probe (the plan's own named test, plan:483-484): assert measured intensity at distance `d` matches the CHOSEN formula's closed-form prediction within tolerance, at both a near-field point (d≪range, where both candidate formulas agree — this alone does not discriminate) AND a point close to `range` (where they diverge by design — this DOES discriminate, and the probe must state which formula it is certifying). |
| Spot cone attenuation (inner/outer angle) | KHR spec reference code, quoted verbatim (README.md:160-169): `lightAngleScale = 1/max(0.001, cos(inner)-cos(outer))`; `lightAngleOffset = -cos(outer)*lightAngleScale`; shader: `angularAttenuation = saturate(cd*scale+offset); angularAttenuation *= angularAttenuation` (squared smoothstep-style falloff). | consume-now | VERIFIED this is the SAME formula Filament's own shader implements: `getAngleAttenuation(lightDir, l, scaleOffset)` (surface_light_punctual.fs:111-114) is `saturate(cd*scaleOffset.x+scaleOffset.y)` then squared — byte-for-byte the same shape as the KHR reference code above (`scaleOffset` precomputed CPU-side, matching KHR's `lightAngleScale`/`lightAngleOffset`). No discrepancy here (contrast with the point/range-windowing row above) — safe to port from EITHER source since they agree. | Unit test: `angleScale`/`angleOffset` computed from a known `(innerConeAngle, outerConeAngle)` pair match the KHR reference code's closed form exactly (device-free); GPU falloff probe asserts a spot light's measured intensity at a sampled point inside/at/outside the outer cone matches the `saturate(...)²` curve's predicted value at that exact angle. |
| Directional light: literally unattenuated | KHR spec, quoted: *"Because it is at an infinite distance, the light is not attenuated."* | consume-now | Trivial but a real, testable invariant — RendererX's existing direct-lighting path already treats the directional light this way (no distance term touches `lightColor`/`lightDirWorld` in `RxDrawData`, draw_data.h:90-92) — Task 13 must not accidentally introduce a distance term when generalizing the shader's light-loop to also handle point/spot. | Regression test: a directional light's measured intensity is IDENTICAL at two probe points at different distances from the scene origin (only point/spot probes vary with distance). |
| Range absent = infinite range, pure inverse-square | KHR spec, quoted: *"When undefined, `range` is assumed to be infinite and the light should attenuate according to inverse square law."* | consume-now | `LightData::range`/`LightRecord::range` are already `std::optional<float>`/a "0.0 = inert" field respectively (mesh_asset.h:257, scene.h:212) — the IMPORTER'S `std::optional<float> range` (only set `if (light.range)`, import_gltf.cpp:1430-1432) already distinguishes "absent" from "present," but `LightRecord::range` is a bare `float` with no such distinction yet (0.0F default is ambiguous with a genuinely-zero range, which the spec forbids anyway — *"Must be > 0"* — so 0.0F can safely double as the sentinel). | Acceptance criterion: `PointLightDesc`/`SpotLightDesc` → `LightRecord` conversion maps `std::nullopt`/glTF-absent range to `LightRecord::range == 0.0F` (the "no windowing, pure 1/d²" sentinel) — a GPU falloff probe at TWO points near the theoretical range-window falloff cutoff of a RANGED light, contrasted against an UNRANGED light at the same two points, discriminates that windowing is actually conditional on `range`'s presence (not always-on). |
| Pre-exposure / range policy dependency on Task 4 | Plan text: *"Pre-exposure/range policy per Task 4's ruling."* (plan:471). | **cross-task dependency, not this ticket's own decision** | Task 4 (Stage 0, camera exposure + physical-units API) has not landed at gate-research time — whether lights are pre-exposed at the SOURCE (Filament's own convention: `computePreExposedIntensity(intensity, frameUniforms.exposure)`, surface_light_punctual.fs:149, applied once per light fetch) or exposure is applied later in the tonemap path is Task 4's ruling to make, binding Stage 1 (IBL) AND this stage. | Not this ticket's acceptance criterion to define — flagged so whoever authors Task 13's own PR does not silently pick a convention Task 4 later contradicts. `RxDrawData`'s existing `lightColor` field (draw_data.h:92) is UN-exposed today (a raw `color*intensity` product) — Task 13's new point/spot intensities must go through the SAME single, documented exposure-application point Task 4 establishes, not a second ad-hoc one. |

---

## Open Questions

1. **Point-light range-window exponent: KHR spec's `(1-(d/range)⁴)` vs. Filament's own shader's `(1-(d/range)⁴)²`.**
   Both are legitimate, well-precedented choices (Filament's squared form gives a
   softer visual falloff near the cutoff), but they are NOT numerically
   interchangeable near `range`, and this gate's own charter states KHR-spec
   exactness is a conformance criterion. **Recommendation: implement the KHR
   spec's literal, un-squared formula** (`max(min(1-(d/range)⁴,1),0)/d²`) as
   the conformance-path default — it is the one a Khronos Sample-Viewer-class
   reference renderer (Task 11's own conformance harness, landing the same
   phase) will be compared against, and "port from Filament's shader code"
   does not override an explicit, independently-stated spec-conformance
   requirement when the two genuinely disagree. If a Filament-matching
   "cinematic" look is later wanted, expose it as a separate, explicitly-named
   variant, never silently substituted for the conformance path.

2. **No public `Scene::createPointLight`/`createSpotLight` API exists today** —
   this is real, necessary, in-scope new work for Task 13 (not a pre-existing
   gap to defer), since the ticket's own acceptance criteria ("point/spot in
   lumens/candela with inverse-square attenuation + radius windowing, spot
   inner/outer cone semantics") are unimplementable without it. **Recommendation:
   build `PointLightDesc`/`SpotLightDesc` mirroring `DirectionalLightDesc`'s
   existing shape+conventions exactly** (same D19-style plain-struct public
   surface, same channels/castsShadows fields) — no new pattern needed, the
   precedent is already in this same file.

## Verification health

**Verified first-hand this session:** every in-repo citation above was read
directly from the working tree 2026-08-20 (`scene.h`, `scene.cpp`,
`mesh_asset.h`, `import_gltf.cpp`, `samples/09_scene/main.cpp`); the
`ImportedScene::lights`-never-consumed claim is a full-repo grep result, not
an inference. The KHR_lights_punctual README was fetched in full from
`KhronosGroup/glTF` at commit `2b29723d025a995971726f2989697cdc49b1222a` and
quoted verbatim (not search-digested). Filament's `LightManager.cpp` and
`shaders/src/surface_light_punctual.fs` were fetched in full from
`google/filament` at commit `721ec800093de984cbee155e459298b6b2dbb855` and
the cited line ranges read directly, not summarized.

**Not independently verified:** whether any OTHER engine (Unreal/Godot)
disagrees with either KHR's or Filament's range-windowing exponent — the
Open Question above is resolved by citing KHR spec primacy directly, so a
third engine's opinion was not sought this session (a scope/budget
trade-off, not an oversight).
