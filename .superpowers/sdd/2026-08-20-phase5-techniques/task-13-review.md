# Task 13 review — Physical light units + punctual lights, KHR_lights_punctual consumption (issue #49)

Independent reviewer round. Commit under review: `5878c7e`
(`feat(rx_scene,rx_material,shaders): physical light units + punctual
lights (#49)`), base `942268b`, branch `task/t13-physical-lights`, worktree
`/media/ywadi/second/renderer_x-worktrees/t13-physical-lights`. Reviewer
did not write this code. Order of authority followed: gate rulings
(`rulings-2026-08-20.md`, T13 + RC7) > brief (`task-13-brief.md`) > gate
matrix (`matrix-p5t13-physical-lights.md`) > ticket (#49).

Every unit-conversion/attenuation formula claim below was independently
re-derived, not trusted from the report: KHR_lights_punctual README.md
fetched fresh at the pinned commit `2b29723d025a995971726f2989697cdc49b1222a`;
`google/filament`'s `LightManager.cpp`, `surface_light_punctual.fs`, and
`IndirectLight.h` fetched fresh at the pinned tag `v1.75.0`
(`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`); `glTF-Sample-Renderer`'s
`punctual.glsl` fetched fresh at the pinned commit
`863b981fb755359063e370ff7b6e956bda0716e2`. The full 142KB diff was read
top to bottom. Both drivers were built/run live in the worktree (warm
build dirs, no rebuild needed for `linux-native`); the revert-discrimination
proof was independently reproduced (sabotage → quantified fail → byte-
identical restore → green); the import-consumption GPU test was re-run on
both drivers; both checkpoint benchmarks were re-measured on real NVIDIA
hardware.

## Verdict 1 — Spec compliance: **PASS** (matrix-p5t13-physical-lights.md, rulings T13 + RC7)

Every matrix row is delivered and value-asserted, and every formula
independently checks out against a fresh pin fetch:

- **Directional (lux), no conversion at import** — confirmed:
  `instantiateImportedLights()` passes `color*intensity` straight through;
  `import_gltf_gpu_test.cpp`'s new TEST_CASE asserts the exact fixture
  values (3.0/2.7/2.4) end to end through a real `registry.importGltf()`.
- **Point/Spot creation API** — `Scene::createPointLight`/`createSpotLight`
  mirror `createDirectionalLight`'s shape exactly, per the matrix's own
  recommendation; round-trip tests pass.
- **No lumen→candela conversion at import** — confirmed by direct code
  read; the matrix's own worked example (`1500` candela imports as exactly
  `1500.0`) is reproduced by the new import-consumption test.
- **Lumens/watts authoring convenience citations** — independently
  re-verified against a fresh fetch of `LightManager.cpp`: `mIntensity =
  efficiency*683.0f*watts` (line 122); POINT `luminousIntensity =
  luminousPower*ONE_OVER_PI*0.25f` = lp/4π (line 364); plain SPOT
  `luminousPower*ONE_OVER_PI` = lp/π (line 390); FOCUSED_SPOT
  `luminousPower/(TAU*(1-cosOuter))` = lp/(2π(1−cosθ)) (line 377). All
  four formulas the report cites match the pinned source byte-for-byte.
  Correctly NOT built (ruling-sanctioned deferral) and not needed for this
  ticket's scope.
- **Point-light range-window attenuation** — confirmed the KHR spec's own
  literal `max(min(1−(d/range)⁴,1),0)/d²` is what shipped
  (`light_math.cpp::rangeAttenuation`, `standard_pbr.slang`'s punctual
  term), matching a fresh fetch of `punctual.glsl`'s `getRangeAttenuation()`
  (identical un-squared form) and Filament's `getSquareFalloffAttenuation()`
  (confirmed via fresh fetch: `smoothFactor*smoothFactor`, i.e. the extra
  square Filament alone applies) — the matrix's Open Question is correctly
  resolved in favor of spec literalism, and the three-way citation is
  accurate.
- **Spot cone attenuation** — the shipped scale/offset squared-saturate
  form matches the KHR reference code exactly (verified against a fresh
  fetch) and, on independent algebraic re-derivation, is **also**
  identical to `glTF-Sample-Renderer`'s `getSpotAttenuation()` — see
  Adjudication 2 below; the report's claim of a divergence there is wrong,
  but the *shipped* formula is correct and, if anything, better-conforming
  than claimed.
- **Directional unattenuated regression** — confirmed:
  `lightType==0` never touches `lightAttenuation` in the shader; GPU
  regression test (`test_standard_pbr_punctual_gpu.cpp`) independently
  re-run, passes.
- **Range absent → infinite range** — confirmed sentinel handling
  (`range<=0.0`) on both the C++ (`light_math.cpp`) and Slang sides,
  matching `glTF-Sample-Renderer`'s identical "negative range unlimited"
  reading.
- **Pre-exposure applied exactly once** — independently re-run
  (`point_pre_exposure` TEST_CASE, lavapipe): measured ratio `1.67213` vs
  expected `1.66667`, and the explicit squared-ratio discriminator did NOT
  match — confirms single application. Grep of every `DrawDataGpu.lightColor`
  producer in the tree (`samples/08_gltf_viewer/main.cpp:2129`,
  `samples/09_scene/main.cpp:2361`) shows exposure multiplied exactly once,
  CPU-side, at upload; the shader never touches an exposure value (the T4
  post-multiply site was deleted, not left dormant, in an earlier round).

No spec-compliance defects found. See Adjudications below for the three
items the dispatch specifically asked to be ruled on.

## Verdict 2 — Code quality: **Approved — 1 MEDIUM (documentation/ledger accuracy, no code change), 2 LOW**

The implementation itself is clean, correctly cited, and the test rig is
genuinely discriminating (verified live, not just re-read): std430 layout
matches field-for-field between `DrawDataGpu` (C++) and `RxDrawData`
(Slang) at the 384-byte boundary with zero implicit padding on either
side; no new descriptor binding or per-draw churn was introduced (the
punctual bundle rides the existing per-draw `StructuredBuffer` row);
pre-exposure discipline is correct and single-application is proven, not
asserted; the revert-discrimination proof reproduces exactly. The one
finding is a factual inaccuracy in the implementer's own narrative
(`task-13-report.md`, now echoed into `progress.md`'s ledger) about a
"spot-cone divergence" from the pinned `glTF-Sample-Renderer` that does
not actually exist — see Finding 1 / Adjudication 2. It requires a
documentation correction, not a re-implementation round: the shipped code
is unaffected and, on the corrected reading, conforms MORE closely to the
cited sources than the report claims (3-for-3 agreement, not 2-for-3).

---

## Adjudications (as specifically requested by the dispatch)

### Adjudication 1 — Reconciliation (`EnvironmentDesc::intensity`, T10-parked item)

**Ruling: coherent, not a hidden unit break.** `EnvironmentDesc::intensity`'s
header comment (`scene.h`) now states unambiguously, at the field
declaration site, that the value is lux-denominated, quoting Filament's
own `IndirectLight::Builder::intensity()` contract verbatim — independently
re-verified against a fresh fetch of `IndirectLight.h:227`: *"Scale factor
applied to the environment and irradiance such that the result is in lux,
or lumen/m^2 (default = 30000)"* — word-for-word match.

Dimensional re-derivation (the report contains no explicit "proof table"
cross-checking a lux-directional-light against an equivalent-luminance
environment in one composed scene — this was independently re-derived
here, not found and re-checked): for a point/spot light, `lightColor
(candela) / distance² = lux`-equivalent irradiance at normal incidence,
exactly the standard photometric point-source formula `E = I/d²`; the
shader's own `directLight = (diffuse+specular) * lightColor *
lightAttenuation * NdotL` composes this with Lambert's cosine law
identically to how the pre-existing directional path already composes
`colorLux` — both terms land in the SAME lux-equivalent-irradiance frame
feeding the SAME BRDF. On the IBL side, `ibl = (iblDiffuse+iblSpecular) *
occlusion * v.envIntensity` (unchanged, pre-existing T10 code) treats
`envIntensity` as exactly the scale-to-lux factor Filament's own contract
describes. The three producers (directional lux, punctual candela/d²,
environment scale-to-lux) are dimensionally coherent with each other.

Keeping the neutral `1.0` default (not Filament's `30000`) is a
**documented, deliberate divergence**, not a silent one: the comment
explicitly names Filament's real value, explains why RendererX does not
adopt it (no source HDR asset in this codebase is calibrated to a
photometric capture standard), and points to the opt-in path (`intensity
= 30000.0F` + a real `Camera` configuration) that recovers Filament's
convention. This mirrors the established `Camera::exposureOverride`
engaged-neutral-by-default pattern from Task 4. No D17 reference
regenerated because no producer's numeric behavior changed — verified:
`git show 5878c7e --stat` touches zero files under any `references/`
directory, and the sample09 D17 gate reproduced `failingPixels=0/65536`
live in this round.

**Gap noted (non-blocking):** there is no dedicated GPU test that renders
a known-lux directional light and a known-intensity environment side by
side and asserts they land in the same output range — the coherence claim
rests on the unit-consistency argument above plus Task 4/10's own
pre-existing exposure tests, not a new empirical cross-check. Worth a
LOW-priority backlog item for whichever task next touches both paths in
the same frame (T14/15), not a T13 blocker — the ticket's own scope never
named this test.

### Adjudication 2 — Spot-cone "divergence" disclosure (implementer concern 1)

**Ruling: the claimed divergence does not exist. The report is factually
wrong on this specific point; the shipped code is correct and conforms to
all three sources, not two of three.**

Fresh fetch of `glTF-Sample-Renderer`'s pinned `punctual.glsl` (commit
`863b981fb755359063e370ff7b6e956bda0716e2`), `getSpotAttenuation()`:

```c
float actualCos = dot(normalize(spotDirection), normalize(-pointToLight));
if (actualCos > outerConeCos) {
    if (actualCos < innerConeCos) {
        float angularAttenuation = (actualCos - outerConeCos) / (innerConeCos - outerConeCos);
        return angularAttenuation * angularAttenuation;   // <-- IS squared
    }
    return 1.0;
}
return 0.0;
```

The report's claim ("a DIFFERENT, simpler formula... no squaring") is
wrong on both counts: the code plainly squares (`angularAttenuation *
angularAttenuation`), and algebraically the linear ramp `(actualCos -
outerConeCos)/(innerConeCos - outerConeCos)` is the *identical* expression
to the KHR reference code's `cd*lightAngleScale + lightAngleOffset`
once expanded (`lightAngleScale = 1/(cosInner-cosOuter)`,
`lightAngleOffset = -cosOuter*lightAngleScale` ⇒ `cd*scale+offset = (cd -
cosOuter)/(cosInner-cosOuter)`), and Sample-Renderer's explicit
`if/else` branching (return 0 below outerConeCos, return 1 above
innerConeCos) is exactly what `saturate(...)` does to that same
expression. These are the same closed-form curve expressed two different
ways — not two different curves. The sign convention also matches:
`actualCos = dot(spotDirection, normalize(-pointToLight))` where
`pointToLight = light.position - worldPosition` is precisely the same
`dot(spotDirWorld, -lightDir)` shape RendererX ports (`lightDir` there is
the "toward light" unit vector, matching `pointToLight`'s normalized
form).

Cross-checked a third way against a fresh fetch of Filament's
`surface_light_punctual.fs` (`getAngleAttenuation`, lines 111-114):
`saturate(cd*scaleOffset.x+scaleOffset.y)` then squared, called as
`getAngleAttenuation(-direction, light.l, scaleOffset)` — the identical
shape again. **All three sources (KHR spec reference code, Filament,
glTF-Sample-Renderer) agree exactly** on the spot-cone falloff shape;
there is no conformance risk of any kind, present or future, from this
port, and no "future light-bearing conformance model" caveat is needed —
this was never a live risk.

This does not affect the shipped code's correctness or the matrix
disposition (still `consume-now`, still correctly ported). It is a
factual error confined to `task-13-report.md`'s narrative ("Spot cone
attenuation" section and "Concerns for the coordinator" item 1) and
`progress.md`'s ledger line summarizing it — grep of every committed
source file under `shaders/` and `src/rx_scene` confirms the false
"divergence"/"no squaring" claim was never written into code comments;
the committed comments correctly cite `glTF-Sample-Renderer` only for the
(genuinely correct) range-attenuation match and sign-convention
cross-check. See Finding 1.

### Adjudication 3 — Scope-phasing (implementer concern 2)

**Ruling: legitimate phase-fit, not a no-deferral violation.** The plan's
own Stage 2 text (`docs/superpowers/plans/2026-08-20-phase5-techniques.md`,
Task 15) states verbatim: *"The lit path consumes cluster lists for
point/spot (directional stays direct)"* — Task 13's own brief/matrix never
requires Point/Spot lights to drive pixels in any sample; the ticket's
acceptance criteria are "authored glTF punctual lights arrive in the Scene
with value-asserted intensities/cones" (satisfied — `scene->lightCount()`
reflects every imported light, of any type, in `populateImportedInstances()`)
and "single-light analytic falloff probe... within tolerance" (satisfied —
directly, via hand-built `DrawDataGpu` rows in
`test_standard_pbr_punctual_gpu.cpp`, not via a sample's production path).
Neither criterion names sample09's own single-light forward/shadow slot.

Verified directly, not taken on the report's word: `grep -n lightType
samples/09_scene/main.cpp` shows `row.lightType = 0` is hardcoded,
unconditional, for every draw row in `updateSceneFrame()` — Point/Spot
lights created by `instantiateImportedLights()` are real, live Scene
objects (`lightCount()`/`lightType()`/`lightColorLux()` all readable) but
structurally cannot reach a pixel through this sample's current draw-data
producer. Separately, the GPU capability genuinely works today, outside
any sample: `test_standard_pbr_punctual_gpu.cpp`'s Point/Spot TEST_CASEs
render real pixels through `standard_pbr.slang` with `lightType=1/2`,
confirming inverse-square falloff, range-window discrimination, and the
spot cone curve — so "capability shipped, sample wiring phased" is
verifiable exactly as claimed, independently reproduced this round on
lavapipe. This is a correctly-scoped, plan-anticipated phase boundary.

---

## Finding 1 (MEDIUM) — spot-cone "divergence" narrative is factually wrong; correct the record, no code change needed

**Where:** `.superpowers/sdd/2026-08-20-phase5-techniques/task-13-report.md`
("Spot cone attenuation" section, "Concerns for the coordinator" item 1)
and the corresponding `progress.md` ledger line ("Disclosed: spot-cone
matches KHR spec+Filament but diverges from pinned Sample-Renderer
shader"). See Adjudication 2 above for the full derivation: the pinned
`glTF-Sample-Renderer`'s `getSpotAttenuation()` squares its result and is
algebraically identical to the KHR reference code's scale/offset
saturate-squared curve RendererX ported — there is no divergence, and the
"no squaring" characterization is directly contradicted by the fetched
source (`return angularAttenuation * angularAttenuation;`).

**Impact:** none on shipped behavior — `shaders/material/standard_pbr.slang`,
`src/rx_scene/light_math.{h,cpp}`, and their test suites are unaffected
and correct. The impact is a false statement propagating into the
permanent SDD record (already echoed once, into `progress.md`), which
could mislead a future task (e.g. T14/15, or a future conformance-model
addition) into believing a real conformance gap exists where none does.

**Recommended fix:** correct `task-13-report.md`'s narrative and the
`progress.md` ledger line to state the three sources agree exactly (no
divergence); no code, test, or shader change required. This can close via
a documentation-only commit/note rather than a full fix round.

## Finding 2 (LOW) — `instantiateImportedLights()`'s switch has no `default` case

**Where:** `src/rx_scene/scene.cpp`, `instantiateImportedLights()`, the
`switch (light.type)` over `asset::LightData::Type`. Currently exhaustive
(3 enumerators, 3 cases) and harmless today. If `LightData::Type` ever
gains a new enumerator (e.g. a future glTF light extension), this switch
would silently produce no handle for that light rather than warning or
asserting, and the function's own doc comment ("Returns one `LightHandle`
per input light, in the SAME order as `lights`") would become violated
silently. Not a live bug; worth a `default:`-path assertion or a
`static_assert`-style enumerator-count check whenever this switch is next
touched. Non-blocking.

## Finding 3 (LOW) — no dedicated env-vs-punctual-light lux cross-check test

Already covered under Adjudication 1's "Gap noted" — restated here only
for the severity ledger. Non-blocking; backlog candidate for T14/15.

---

## Baseline-drift numbers (NVIDIA GeForce RTX 2080, driver 580.82.07, `--bench-frames 200 --validate`, `cpu_record_avg_ms`)

| Scene | Stage 1 checkpoint (4d52d8f) | This round, independently re-measured | Δ |
|---|---|---|---|
| DamagedHelmet | 0.219 ms | 0.212 ms | **−3.2%** |
| Sponza | 4.547 ms | 4.259 ms | **−6.3%** |

Both scenes are comfortably within noise and show a slight *improvement*,
not a regression — the `+432`-byte `DrawDataGpu` growth (one extra
`StructuredBuffer` row's worth of bytes per draw, no new binding, no new
per-draw descriptor work) does not move the Stage 1 baseline in either
direction beyond normal run-to-run variance. (Note: an initial pass of
this benchmark without `--validate` measured `cpu_record_avg_ms=0.015` for
helmet — a red herring from omitting the flag the checkpoint methodology
requires; validation-layer instrumentation dominates `cpu_record`'s own
CPU-side command-recording cost far more than any T13 change does. Re-run
with `--validate` reproduces the checkpoint's own measurement conditions
and lands within 3-6% both ways.)

## Empirical verification (driver-labeled, all independently re-run this round)

- **Full ctest, lavapipe**: 42/42 passed, 136.96s, zero unfiltered
  validation errors (419 hits on a raw `--validate` grep, all "known false
  positive"-labeled pre-existing classes; zero unlabeled).
- **Full ctest, NVIDIA RTX 2080 (580.82.07)**: 42/42 passed, 230.01s.
- **Conformance harness (`rx_conformance_*`, 8 tests)**: green both
  drivers, included in the above full-suite runs.
- **Import-consumption GPU case vs `cube_lights_camera.gltf`**: re-run
  standalone on both drivers — lavapipe 1/1 test case, 35/35 assertions;
  NVIDIA 1/1, 35/35. Both pass.
- **Revert-discrimination, independently reproduced**: sabotaged
  `standard_pbr.slang`'s range-window block (commented out the windowing
  branch, `rangeWindow` forced to `1.0`) → re-ran
  `test_standard_pbr_punctual_gpu.cpp` → 2 CHECKs failed exactly as the
  report claims (`rangedRatio` collapsed from `8.71` to `3.3`, exactly
  equal to `unrangedRatio`) → restored the file via `Edit` back to the
  original text → `git diff` on the file empty (byte-identical) → re-ran →
  5/5 test cases, 403/403 assertions, green (lavapipe). `git status`
  clean throughout (only the pre-existing worktree-convention symlinks
  `.deps-cache`/`assets/fetched`/`toolchain` untracked, unrelated to this
  commit).
- **D17 regression gate, sample09, lavapipe**: `failingPixels=0/65536`,
  reproduced live — confirms the byte-identical-for-pre-existing-
  compositions claim directly, not by trusting the report.
- **DrawDataGpu/RxDrawData layout**: confirmed field-for-field identical
  order/types between `src/rx_material/include/rx_material/draw_data.h`
  and `shaders/material/material.slang`'s `RxDrawData`; both builds
  compiled clean with the `static_assert(sizeof(DrawDataGpu) == 432, ...)`
  in place (a layout mismatch would have failed to compile, not just
  misbehaved at runtime).

## Not independently verified this round

- **`windows-cross-zig`/Wine (14/14)**: not re-run — outside this
  dispatch's explicit empirical-minimum list (GPU-backed tests are
  excluded from that CI lane by design; nothing punctual-specific runs
  under Wine). Taken on the report's word only.
- **Steam Deck numbers**: not applicable this round (RC8 posture,
  honest-manual, no Deck hardware in the loop — unchanged from every
  prior Phase 5 round).
- **The "proof table" for env-vs-punctual lux equivalence** referenced in
  this dispatch's own attention lens does not exist as a discrete
  artifact in `task-13-report.md` — see Adjudication 1; a dimensional
  re-derivation was performed independently in its place, and a gap
  (Finding 3) was logged for the missing empirical cross-check.

## Hygiene

Single commit (`5878c7e`) on `task/t13-physical-lights`, author `Yousef
Wadi <ywadi85@gmail.com>` (confirmed via `git log`), zero AI attribution
anywhere in the commit message, diff, or touched files (grepped for
`claude`/`anthropic`/`co-authored-by`/`generated with`, case-insensitive,
zero hits). Not pushed (`git ls-remote --heads origin` has no
`task/t13-physical-lights` ref). `main`/`origin/main` untouched, still at
base `942268b`. Fixture provenance: `assets/test/cube_lights_camera.gltf`
predates this commit (`git log --follow` traces it to
`337d368 feat(rx_asset): glTF 2.0 import core`, not touched by `5878c7e`'s
own diff) — the report's "pre-staged, reused as-is" claim is accurate.
Worktree left clean (only the standing convention symlinks untracked); no
temporary edits remain (the sabotage edit used for the revert-
discrimination re-proof was restored byte-identically and verified via
`git diff`).

---

## Re-review (fix round 1)

Scoped re-review. Commit under review: `e63ee06`
(`fix(rx_scene,rx_material): review fix round 1 -- loud default arm,
env/punctual coherence proof (#49)`), on top of `5878c7e`, same branch
(`task/t13-physical-lights`), same worktree. Scope: closure of this
review's three findings only. `task-13-report.md`'s "Fix round 1" section
was read first, then every claim was independently re-verified in the
worktree (not trusted from the report), same as the first round.

### Finding 1 (MEDIUM, spot-cone erratum) — **CLOSED**

`task-13-report.md`'s "Spot cone attenuation" section and "Concerns for
the coordinator" item 1 both now state the accurate conclusion: all three
sources (KHR reference code, Filament, `glTF-Sample-Renderer`) agree
exactly on the squared scale/offset curve, with the same algebraic
derivation this review supplied (Adjudication 2) reproduced correctly and
attributed. Item 1 is struck through (`~~...~~`) and marked "RETRACTED,
Fix round 1" with the correction inline, not silently deleted — the
erratum stays visible in the permanent record as intended. No
overcorrection: the fix does not introduce any new unsupported claim, and
correctly states "there is no conformance risk, present or future" rather
than overstating certainty beyond what the derivation supports. The
per-row proof table's "Spot cone attenuation" row was updated
consistently ("matches ALL THREE sources exactly... corrected per Fix
round 1, no divergence"). No code, test, or shader diff — confirmed via
`git show e63ee06 --stat`: only `src/rx_scene/scene.cpp` and
`test_standard_pbr_punctual_gpu.cpp` changed; `standard_pbr.slang` and
`light_math.{h,cpp}` are untouched by this commit, correctly, since they
were never wrong.

### Finding 2 (LOW, switch default) — **CLOSED**

`src/rx_scene/scene.cpp`'s `instantiateImportedLights()` switch gained a
`default:` arm (`RX_LOG_ERROR(...)` naming the unhandled raw enum value,
then `throw std::logic_error(...)`). Confirmed this matches the
codebase's own loud-failure idiom by direct comparison against
`Scene::requireLiveRenderable()` (same file, lines 33-40): `RX_LOG_ERROR`
with context, then `throw`. This is the correct idiom for `asset::
LightData::Type` — an internal, engine-owned enum, not externally-sourced
malformed data (which this codebase instead handles via a WARN-log-and-
degrade idiom elsewhere, e.g. `mapFastgltfError()`) — so log+throw
(fail loud, don't silently drop a light) is the right choice, not the
wrong one. The three real enumerators (`Directional`/`Point`/`Spot`) are
unaffected — the `default:` arm is unreachable in every existing call
path, confirmed empirically: `rx_scene_tests` 93/93 (6681/6681
assertions) on both lavapipe and NVIDIA, and the import-consumption GPU
TEST_CASE against `cube_lights_camera.gltf` (which exercises all three
real light types through this exact switch) re-run standalone on
lavapipe: 1/1, 35/35 assertions, clean.

### Finding 3 (LOW, coherence test) — **CLOSED**

The new TEST_CASE's derivation is the one from Adjudication 1: a uniform
environment of radiance `L` (isolated to `iblDiffuse==diffuseColor*L`
exactly via `dfg=(0,0)`, zeroing `iblSpecular`) is compared against a
directional light of `colorLux = L*envIntensity*π` — the standard
Lambertian hemisphere-irradiance identity (`E = π*L` for a uniform-
radiance hemisphere), the exact "candela/d² and colorLux land in the same
lux-equivalent-irradiance frame" argument this review made independently
(re-derived from first principles in this round, not found and merely
re-checked). Independently re-traced the algebra with `ior=1.0` forcing
`F0=0` at this rig's head-on geometry (`VdotH==1` ⇒ Schlick's `p5` term
vanishes ⇒ specular exactly zero): `directLight == C/π == L*envIntensity`
on the direct side, `ibl == L*envIntensity` on the env side (with
`occlusion==1`, white material) — both pre-exposed by the identical
single CPU-side multiply. The two sides are the same quantity by
construction, not a coincidental numeric match.

**Tolerance justified**: `±0.06×` (≈±6/255 at the neutral pixel value of
102) is generous enough to absorb any fp16-texture-path-vs-fp32-direct-
path rounding drift but far tighter than the ~12.6× the sabotage
introduces — independently re-confirmed empirically below, not just
argued.

**Re-run, both drivers** (`--test-case="StandardPBR: environment-lux*"`,
`--validate`):

| Exposure | env pixel | directional pixel | lavapipe | NVIDIA RTX 2080 (580.82.07) |
|---|---|---|---|---|
| 1.0 (neutral) | 102 | 102 | match | match |
| 1.66667 (`setExposure(-1.0F)`) | 170 | 170 | match | match |

Identical numbers on both drivers, identical to the report's claim.
1/1 test case, 133/133 assertions, both drivers.

**Sabotage discrimination, independently reproduced**: edited
`standard_pbr.slang`'s `directLight` expression to add a stray `/
12.56637061` (4π) divisor, re-ran the same TEST_CASE (lavapipe, no
rebuild — Slang compiles in-process):

```
env-vs-punctual coherence @ exposure=1.0: env=102 directional=8
ERROR: CHECK( near(dirNeutral.r, envNeutral.r, 0.06F) ) is NOT correct!
env-vs-punctual coherence @ exposure=1.66667: env=170 directional=14
ERROR: CHECK( near(dirBright.r, envBright.r, 0.06F) ) is NOT correct!
[doctest] test cases: 1 | 0 passed | 1 failed | 74 skipped
```

Exact match to the report's own claimed numbers (102 vs 8, 170 vs 14).
Restored via `Edit` back to the exact original line; `git diff --stat`
on the file empty (byte-identical); re-ran `rx_material_gpu_tests`
(lavapipe, full suite, `--validate`): 75/75 test cases, 4092/4092
assertions, zero unfiltered validation errors. `git status --short`
clean throughout (only the pre-existing worktree-convention symlinks
`.deps-cache`/`assets/fetched`/`toolchain` untracked).

**Squared-exposure discrimination — reasoned, not re-sabotaged** (the
dispatch asked for reasoning here, and re-running the 4π case
empirically instead, which was done): at neutral exposure (`1.0`), `x ==
x²`, so a stray squared-exposure bug on either producer is
indistinguishable from correct behavior at that data point alone — the
neutral-exposure assertion cannot catch it. At the non-neutral exposure
used here (`5/3 ≈ 1.667`), a producer that squared its own exposure
multiplier instead of applying it linearly would compute `L*envIntensity
*exposure²` ≈ `0.4*2.778 ≈ 1.111` → clipped to `255` (saturated),
against the correctly-linear producer's `0.4*1.667 ≈ 0.667` → `170` — a
`255` vs `170` divergence, ~50% relative, an order of magnitude past the
`±0.06×` tolerance and trivially visible at 8-bit precision. **Conclusion:
yes, the test as constructed would catch a squared-exposure regression on
either producer** — the non-neutral exposure level is load-bearing for
exactly this reason, matching the report's own claim; the neutral-exposure
half alone would not be sufficient, which is why both are asserted.

### Overall verdict: **ALL ADDRESSED**

All three findings from the original round are closed: 1 MEDIUM (report/
ledger-only correction, verified accurate and non-overcorrected, zero
code impact) + 2 LOW (both closed in code, both independently re-verified
live on both drivers with a real sabotage/restore cycle for the new test).
Spec compliance verdict remains **PASS**; code quality verdict is now
**Approved, clean** (no open findings).

### Hygiene (`e63ee06`)

Single commit on `task/t13-physical-lights`, author `Yousef Wadi
<ywadi85@gmail.com>` (confirmed via `git log`), zero AI attribution
(grepped commit message + full diff, case-insensitive, zero hits).
Pathspec-scoped to exactly the two files the fix claims (`git show
e63ee06 --stat`: `src/rx_scene/scene.cpp` + `test_standard_pbr_punctual_
gpu.cpp`, no stray changes). Not pushed (no matching remote branch via
`git ls-remote --heads origin`). `main`/`origin/main` untouched, still at
base `942268b`. Both checkouts clean at the end of this round: the
worktree shows only the pre-existing convention symlinks untracked (no
diff, no stray files); the main checkout shows only the pre-existing,
intentionally-untouched `progress.md` modification that predates this
entire review (this file's own gitignored `.superpowers/sdd/` location
means this append itself is invisible to `git status`).
