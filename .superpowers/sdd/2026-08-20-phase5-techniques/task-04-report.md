# Task 4 report — Camera exposure + physical-units API (issue #40)

Implementer round. Base: main `26db15a`. Order of authority followed:
rulings (`rulings-2026-08-20.md`, "T4 (#40)") > plan (Task 4) > gate matrix
(`matrix-p5t04-camera-exposure.md`) > ticket (#40).

## Status: COMPLETE

All matrix rows (as ruled) satisfied. Both presets green. Real-driver
(NVIDIA GeForce RTX 2080) and lavapipe full-suite runs both clean, zero
unfiltered validation errors. Default (`exposure=0.0`/no-flag) output
proven byte-identical on lavapipe — no reference regeneration.

## What shipped

**`rx::scene::Camera` gains a physical-units exposure API
(`src/rx_scene/include/rx_scene/camera.h`/`camera.cpp`), superseding D22
wholesale:**
- `rx::scene::exposure::{ev100, exposure}` — pure free functions ported
  byte-for-byte from Filament's `Exposure.cpp` (google/filament **v1.75.0**,
  commit `0e58877c09afb1aacd09ff640f74d2adcd2a7e80`, Apache-2.0 — fetched
  and diffed against this exact tag via `gh api ...?ref=v1.75.0` this
  session, not paraphrased from docs; tag SHA independently verified via
  `gh api repos/google/filament/git/refs/tags/v1.75.0` to match RC1's pin).
  `ev100(N,t,S) = log2((N²/t)·(100/S))`; the merged
  `exposure(N,t,S) = 1/(1.2·(N²/t)·(100/S))`; `exposure(ev100) =
  1/(1.2·2^ev100)`. The `1.2` constant is the exact saturation-based-
  sensitivity derivation (`78/(100·0.65)`), not a tunable — cited in the
  header comment, matching this codebase's own constant-citation
  discipline.
- `Camera::aperture`/`shutterSpeed`/`sensitivity` — Filament's own
  `Camera.h` defaults verbatim (`16.0F`, `1/125F`, `100.0F`,
  `details/Camera.h:211-213` v1.75.0).
- `Camera::ev100()` / `Camera::exposure()` — the triple-derived EV100 and
  the pre-exposure multiplier every light/IBL-intensity producer this
  ruling binds must apply to its own values.
- `Camera::exposureOverride` (`std::optional<float>`, defaults engaged at
  exactly `1.0F`) — the mechanism resolving the matrix's own flagged
  tension (row 1 vs. row 4/Conflicts row 2): Filament's real photographic
  defaults do NOT resolve to a neutral multiplier (`ev100≈14.97`,
  `exposure≈2.6e-5`), so the override — not the triple — is what makes a
  freshly-constructed `Camera::exposure() == 1.0` exactly, the load-bearing
  D17 regression default.
- `Camera::setExposure(aperture, shutterSpeed, sensitivity)` — Filament's
  exact clamp ranges ported (`details/Camera.cpp:45-50`: aperture
  [0.5,64], shutterSpeed [1/25000,60]s, sensitivity [10,204800] ISO);
  clears the override.
- `Camera::setExposure(float ev100Override)` — direct override overload
  (matrix row 1's own proposed shape; row 6's "no literal Filament
  Camera-level precedent" flagged and documented — only Filament's free
  `Exposure::exposure(float ev100)` exists, never wired onto `FCamera`).
- `Camera::clearExposureOverride()` — restores the same neutral state a
  fresh `Camera` starts with.

**Filament-style PRE-EXPOSURE adopted (ruling, matrix row 3), not a
post-tonemap/post-shading multiply:**
- `material.slang`'s `RxMaterialGlobals` push-constant struct LOST its
  `exposure` field entirely (was: `defaultSamplerIndex`,
  `drawDataBufferIndex`, `exposure`; now: the first two only — 8 bytes,
  not 12). `rx::material::MaterialGlobalsPush` (`draw_data.h`) mirrors
  this exactly, `static_assert`-pinned at 8 bytes.
- `forward_entry.slang`'s fragmentMain — the old
  `color.rgb *= exp2(gMaterialGlobals.exposure);` post-multiply is
  **deleted**, not left dormant (matrix row 5's own explicit "double-
  application" failure-mode warning).
- Exposure now multiplies each `RxDrawData` producer's own
  `lightColor`/`ambientColor` BEFORE upload — "at the source", matching
  Filament's `View.cpp` (`prepareAmbientLight`/`prepareDirectionalLight`,
  both fed a single per-frame `exposure` computed from `cameraInfo.ev100`,
  verified first-hand against v1.75.0 this session).
- `samples/08_gltf_viewer/main.cpp`: `App::exposureCamera` (a real
  `rx::scene::Camera`) replaces the old bare `float exposure` field.
  `updateDrawDataPerPassFields()` scales its own `lightColor`/
  `ambientColor` locals by `exposureCamera.exposure()` before writing
  each row. `--exposure`'s CLI flag is preserved AS A FLAG, but its VALUE
  now feeds `Camera::setExposure(float ev100Override)` directly (matrix
  row 5's own sanctioned mapping) — **with one deliberate, documented
  sentinel**: `args.exposure == 0.0F` (the flag's own pre-Task-4 "0 ==
  neutral" default) does NOT call `setExposure()` at all, leaving
  `exposureCamera` at its own default-constructed neutral state
  (`exposure()==1.0` exactly) — see "Deviations" for why this, not
  "feed 0.0 through the formula", is what the byte-identical regression
  guard actually requires. Shared by both entry points via a new
  `applyExposureArg()` helper.
- `samples/09_scene/main.cpp`: wired for coherence (no half-migrated
  consumer) even though this sample has no `--exposure` CLI control of
  its own — `updateSceneFrame()` now scales its own `lightColor`/
  `ambientColor` locals by `app.flyCamera.camera.exposure()` (its own
  real `rx::scene::Camera`, `fly_camera.h`), a no-op today (always
  neutral) but future-proofed.
- `material_system.cpp`'s `bindInstance()` legacy default-row path: just
  drops the now-nonexistent `push.exposure = 0.0F;` line (was never wired
  to `--exposure` control either way — unaffected by the ruling).

**Tests added/rewritten (all passing, both presets, both drivers):**
- `src/rx_scene/tests/camera_test.cpp` — 6 new device-free `TEST_CASE`s:
  a 5-triple `ev100()`/`exposure()` formula table (bright daylight through
  bright-sun-narrow-aperture, `doctest::Approx(...).epsilon(0.0001)`,
  matching this file's own established tolerance idiom) against
  independently-computed (python3, double precision) reference values;
  Filament-default-triple pin; the neutral-1.0-default proof (explicitly
  distinguishing it from what the triple's own math would give);
  `setExposure(triple)` clamp-range + override-clearing proof;
  `setExposure(ev100Override)` triple-independence proof; `clearExposureOverride()`
  round-trip proof.
- `src/rx_material/tests/test_standard_pbr_unlit.cpp` — the old
  "`--exposure`: 2^exposure pre-tonemap multiply" `TEST_CASE` (which
  pushed a push-constant scalar, now impossible) is **replaced**, not
  patched, by "Camera pre-exposure: ..." — a genuinely value-asserted
  GPU test: isolates the ambient term to an exact closed form
  (`color == ambientColor·occlusion(1.0)·baseColor`, zero direct light),
  drives it with two real `rx::scene::Camera` instances (neutral default,
  and `setExposure(-1.0F)` → `exposure()==5/3` exactly), asserts the
  EXACT expected 8-bit pixel values (51 and 85, both exact rational
  fractions of 255, not hand-waved), AND asserts the measured brightness
  RATIO matches `exposure()`'s own ratio (not its square) — the explicit
  double-application-revert discriminator the matrix's own "New gaps"
  section flagged as having no existing precedent in this codebase.
  `DrawRequest`/`renderOne()` lost their now-meaningless `exposure`
  parameter; every other call site (46 others) unaffected since the
  parameter always defaulted to the now-neutral no-op value.
  `rx_material_gpu_tests` gained a `rx_scene` link dependency (CMakeLists)
  for this one test.
- `src/rx_material/tests/test_standard_pbr_shadow_gpu.cpp`,
  `src/rx_material/material_system.cpp`,
  `samples/09_scene/main.cpp` — dead `push.exposure = 0.0F;` /
  `MaterialGlobalsPush{..., 0.0F}` sites removed (would not compile
  otherwise; `MaterialGlobalsPush` is now a 2-field aggregate).

## Per-row proof (matrix)

| # | Criterion | Disposition | Evidence |
|---|---|---|---|
| 1 | `Camera` gains aperture/shutter/ISO + EV100/exposure helpers + direct override, replacing tonemap-side `--exposure` | Delivered | `camera.h`/`camera.cpp`; Filament defaults verbatim; `setExposure` overload pair |
| 2 | EV100/exposure math matches Filament reference values | Delivered | `camera_test.cpp`'s 5-triple table, `epsilon(0.0001)`, `1.2` constant cited with derivation |
| 3 | Pre-exposure vs. full-float ruling | **Ruling adopted verbatim** (pre-exposure) | `RxMaterialGlobals`/`MaterialGlobalsPush` exposure field removed; light/ambient scaled at source in both samples |
| 4 | Neutral-value regression guard: default reproduces Phase 4 output byte-identically | Delivered, verified not assumed | `defaultCamera.exposure()==Approx(1.0F)` (device-free) + lavapipe D17 gates 0/65536 failing pixels, both samples 08/09 |
| 5 | Exposure applied exactly once, pre-tonemap, documented pipeline point; `--exposure` migrates with behavior preserved | Delivered | Old post-multiply site deleted (not dormant); linearity (not squared-ratio) GPU test; CLI flag preserved with documented sentinel semantics |
| 6 | `setExposure` direct-override API shape | Delivered per matrix's own recommendation | `setExposure(float)` overload, `exposureOverride` mechanism, documented as a RendererX extension beyond literal Filament `Camera` |

## Deviations from the matrix's stated expectation

**Row 4's own Conflicts #2 flagged that Filament's real camera defaults
(16, 1/125, 100) do NOT resolve to `exposure()==1.0`, and left the exact
resolution mechanism to the implementer.** Resolved via
`exposureOverride` defaulting ENGAGED at `1.0F` (not derived from the
triple) — the triple itself still holds Filament's real defaults (so
`ev100()` reads a genuine `~14.97` out of the box), but `exposure()`
(what actually reaches `RxDrawData`) reads neutral until a caller
explicitly calls `setExposure(...)`. This is the ONLY design that
satisfies both row 1 ("Filament's exact defaults as RendererX's own
defaults") and row 4 ("must resolve to a multiplier of exactly 1.0")
simultaneously — verified directly, not asserted, by
`camera_test.cpp`'s "default-constructed Camera resolves to a neutral
(1.0) multiplier despite its non-neutral photographic defaults" case.

**Sample 08's `--exposure` CLI flag: `0.0F` is a sentinel, not "ev100=0
fed through the formula".** Matrix row 5 sanctions feeding the flag's raw
value into `Camera::setExposure(ev100Override)` directly — but doing that
literally for the flag's own DEFAULT value (`0.0F`, unchanged from Phase
4, meaning "0 == neutral") would resolve to `exposure(0.0)≈0.833`, not
`1.0`, silently breaking the D17 byte-identical default. `applyExposureArg()`
therefore skips the `setExposure()` call entirely when `args.exposure ==
0.0F`, leaving `exposureCamera` at its own default neutral state. This is
the one place this task's own CLI-flag migration is NOT a literal
1:1 formula feed — documented at three sites (`Args::exposure`,
`App::exposureCamera`, `applyExposureArg()`) so a future reader does not
mistake it for an oversight. Verified empirically: `sample_08_gltf_viewer
--validate` (no `--exposure`) on lavapipe reproduces its committed
reference at 0/65536 failing pixels for BOTH captured frames;
`--exposure -1.0` (a real, non-neutral override, `exposure()==5/3`)
measurably diverges (5598/65536, 8.54%) confirming the plumbing is live,
not merely a no-op.

**No reference-PNG regeneration.** Both sample 08 (`loading_state` +
`loaded_scene`) and sample 09 (`grid_scene`) D17 gates measured
`failingPixels=0/65536 (0.0000%)` on lavapipe with this task's changes,
zero regeneration run.

## Both-preset / both-driver verification (command tails)

Lavapipe, full suite, linux-native:
```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --test-dir build/linux-native --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  80.69 sec
```

Real driver (NVIDIA GeForce RTX 2080, default ICD, full suite):
```
ctest --test-dir build/linux-native --output-on-failure -j4
...
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  69.97 sec
```
D17 gate specifically, real driver: `loading_state` failingPixels=0/65536
pass=true; `loaded_scene` failingPixels=425/65536 (0.6485%) pass=false
`[non-lavapipe driver -- informational only, not enforced]`; overall
`headless gate PASSED` — this is PRE-EXISTING Phase-4 driver-divergence
behavior (same shape T3's report already documented for its own change),
unrelated to this task.

Windows-cross-zig, full build + Wine ctest (T2/T3's own exclusion
pattern):
```
ninja                                    # 38/38, zero errors
xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample' --output-on-failure
...
100% tests passed, 0 tests failed out of 13
Total Test time (real) = 123.43 sec
```

Direct lavapipe D17 re-verification (both samples, isolated ICD):
```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./sample_08_gltf_viewer --validate
  D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
  D17 loaded_scene gate:  failingPixels=0/65536 (0.0000%) pass=true
  headless gate PASSED

VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./sample_09_scene --validate
  D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
  headless gate PASSED

VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./sample_08_gltf_viewer --validate --exposure -1.0
  D17 loaded_scene gate: failingPixels=5598/65536 (8.5419%) pass=false   # expected: real exposure change, not a bug
```

`rx_material_gpu_tests --validate`: 57/57 test cases, 2549/2549
assertions, zero unfiltered validation errors (every "[vulkan validation]"
line matches this codebase's own pre-existing documented false-positive
filter set).

## Self-review

- **TDD discipline**: exact Filament formulas fetched and verified
  first-hand (`gh api`) BEFORE any implementation code was written;
  `camera_test.cpp`'s table test's expected values computed independently
  (python3) from the cited formulas, not reverse-fitted from the port's
  own output; the pre-exposure GPU test's expected pixel values (51, 85)
  derived algebraically from the exact closed form before running, then
  confirmed to match.
- **No deferred fixes**: every `MaterialGlobalsPush`/`RxMaterialGlobals`
  call site touched by the field removal was found and fixed in this
  round (9 additional aggregate-init sites in
  `test_standard_pbr_unlit.cpp` beyond the ones first spotted by grep —
  caught by the build failing, not missed).
- **Revert-discrimination**: the pre-exposure GPU test's ratio assertion
  is the explicit double-application-revert proof (a stray leftover
  post-multiply would measure the SQUARE of the expected ratio, not the
  ratio itself) — this is a genuinely new test pattern for this codebase,
  as the matrix's own "New gaps" section flagged.
- **No AI attribution**: none added anywhere (commit messages, code
  comments, this report).
- **Commit scope**: pathspec-scoped to exactly the files listed under
  "What shipped" above; `.superpowers/sdd/2026-08-20-phase5-techniques/
  progress.md` is concurrently modified by another agent in this shared
  tree (confirmed via `git status` throughout this session, not present
  in this task's own diff) and is deliberately excluded from this commit.
- **Scope discipline**: no Stage 1 IBL / Stage 2 physical-light work
  attempted — this task lands the `Camera` API + the pre-exposure
  CONVENTION only, exactly as the matrix's own row 5 scopes it ("Task 4
  itself has no direct lighting to pre-expose yet").
- **Concerns for the coordinator**: (1) `setExposure(float ev100Override)`
  has no literal 1:1 Filament `Camera`-class precedent (row 6's own
  finding, confirmed independently this session — only the free
  `Exposure::exposure(float ev100)` function exists) — flagged in code
  comments as a RendererX extension, not misrepresented as a direct port;
  (2) sample 08's `--exposure` flag semantics changed from "2^x brightness
  multiplier" to "real EV100 stops" (higher now DARKENS, the real-camera
  convention) for every NON-zero value — a deliberate, matrix-sanctioned
  change (row 5), but worth the coordinator's awareness since it is a
  user-facing behavior change to a documented CLI flag, even though the
  zero/default case is untouched; (3) Stage 1 (IBL, Tasks 9-10) and Stage
  2 (physical lights, Task 13) are the ruling's actual downstream
  consumers of this convention — neither exists yet, so this task's
  pre-exposure wiring currently touches only the two samples' own interim
  flat-ambient/key-light terms, not a real light/IBL system.
