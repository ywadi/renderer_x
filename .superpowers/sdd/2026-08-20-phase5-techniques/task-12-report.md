# Task 12 report — Stage 1 exit: viewer upgrade + checkpoint numbers (issue #48)

Authority order followed: `gate/rulings-2026-08-20.md` T12 section + RC7/RC8
> `task-12-brief.md` > `gate/matrix-p5t12-stage1-exit.md` > ticket (#48).
Worktree: `renderer_x-worktrees/t12-stage1-exit`, branch `task/t12-stage1-exit`,
base `92eae34`.

## Status: DONE

`08_gltf_viewer` is the Stage 1 demonstrator: environment switching
(`--env`/`--no-env`/`--env-intensity`, already landed by Task 10 and
confirmed unmodified this round), Task 4's exposure controls surfaced via
`--exposure` (already landed, confirmed unmodified), and a **new** ImGui HUD
(Task 12's own real scope addition) reporting both live. Packaging, CI
comments, README/MANUAL_VERIFICATION rows are updated. Three **real,
pre-existing** packaging defects (not introduced this round, inherited from
Tasks 7/8/10) were found by this task's own standalone-zip verification and
closed in-round per the no-deferred-fixes standing rule. Both presets green,
real-NVIDIA + lavapipe + Wine all green, packaged zip standalone-verified
end to end (fresh unzip, outside the build tree, all nine samples). Stage 1
numbers (IBL bake timings + frame times for helmet/Sponza/Workshop, both
drivers) are below.

## What was actually new vs. already-landed (important scoping note)

The gate matrix (`matrix-p5t12-stage1-exit.md`) was written **before** Task
10 landed and assumed `--env` and HUD-free exposure were still open gaps.
By this round's base (`92eae34`, post-T10/EXR/T11), `--env`/`--no-env`/
`--env-intensity` and `--exposure` (real EV100, `Camera::setExposure()`)
were **already fully implemented** in `08_gltf_viewer` — grep-confirmed
before touching anything (`git log` shows `3f62df1`/`1a71e9c`/`8dab009`
delivered these). T12's own real, net-new scope was therefore narrower than
the matrix's own rows implied:

1. **HUD environment/exposure readout** — genuinely absent; built this round.
2. **Packaging** — genuinely incomplete; three real gaps found and closed
   (below), none of them the ones the matrix predicted (it expected a
   missing HDR fixture + a missing third reference PNG; both of those were
   actually already handled by Task 10's own packaging additions — the real
   gaps were shader-file omissions the matrix had no way to know about).
3. **CI** — no new mechanism needed. `--env`'s default resolution is
   already exercised by the existing `sample_08_gltf_viewer_headless` test
   (env-path is not gated behind a flag), and Task 6's EXR routing test
   already proves container-format switching. RC8's CI perf-regression-gate
   *mechanism* is explicitly T36's job, not T12's (confirmed against RC8's
   own text: "T36 BUILDS the CI perf-regression gate mechanism").
4. **Stage checkpoint numbers** — genuinely new measurement work; done
   this round (see Numbers section).

## What shipped

### 1. HUD (`samples/08_gltf_viewer/main.cpp`)

- `App::overlay` (`std::optional<rx::debug_ui::Overlay>`) — the SAME engine
  HUD facility `09_scene` already consumes (`rx_debug_ui`, gate ruling #16).
  No sample-local ImGui-context/font/text-rendering code of any kind.
- `drawHud(App&, bool presentMode, double lastFrameMs)` — new function,
  built entirely on `ImGui::*` calls (the sanctioned seam) plus read-only
  reads of `App::environment`/`App::environmentPath`/`App::envIntensityPhysical`
  and `App::exposureCamera`'s own public API (`aperture`/`shutterSpeed`/
  `sensitivity`/`ev100()`/`exposure()`). Two sections: **Environment**
  (bound path, physical intensity, prefiltered mip count, or "none bound"),
  **Exposure** (aperture/shutter/ISO, EV100, resulting pre-exposure
  multiplier, and whether `--exposure` engaged a direct override).
- Wired into both entry points:
  - `runHeadless()`: overlay created before `declareGraph()`; the two
    existing D17-gated captures (`loading_state`/`loaded_scene`) call
    `beginFrame()` with **no widgets drawn** (byte-identical output,
    verified — no reference regen needed); a new, non-gated HUD
    smoke-test frame (mirrors `09_scene`'s own established pattern) draws
    real widgets and asserts `ImGui::GetDrawData()` is non-empty.
  - `runPresent()`: overlay created before `declareGraph()`; every SDL
    event is routed through `overlay->processEvent()` first (gate ruling
    #16 ordering); `beginFrame()` + `drawHud(..., presentMode=true, dt)`
    run once per frame before `PresentLoop::runFrame()`.
- `declareGraph()`: `app.overlay->addPass(graph, "backbuffer")` declared
  AFTER `"tonemap"` (LOAD, not CLEAR — same automatic-derivation contract
  `09_scene` already relies on).
- `destroyApp()`: `app.overlay.reset()` added, positioned identically to
  `09_scene`'s own (right after the initial `vkDeviceWaitIdle`, before
  bindless/device teardown).

**Real bug found and fixed by this round's own screenshot verification**
(not by code review alone): the first draft's "direct EV100 override
engaged" HUD annotation read `exposureCamera.exposureOverride.has_value()`
directly — but that optional is **engaged by default** at a neutral 1.0F
even on a freshly-constructed `Camera` (see `Camera::exposureOverride`'s own
header comment, Task 4's own design). Reading it directly mislabeled every
unmodified run ("nobody passed `--exposure`") as having an explicit
override engaged. Fixed by adding `App::exposureOverrideFromCli` (a plain
`bool`, set only in `applyExposureArg()` when `args.exposure != 0.0F`) and
reading that instead. Screenshot evidence of both the bug and the fix is in
this report's Verification section.

### 2. `--bench-frames <n>` (new CLI flag, `samples/08_gltf_viewer/main.cpp`)

Headless-only. After the scene loads and the environment bakes, times `n`
repeated offscreen `captureFrame()` iterations (the SAME existing
submit-and-wait path the D17 gate itself uses) and logs
`sample08: perf frame_bench scene='...' env='...' frames=N avg_ms=X
min_ms=Y max_ms=Z`. `0` (default) disables it — zero cost to the existing
D17 gate. This is CPU-record + GPU-submit-and-wait wall time, **not**
vsync-paced present-mode timing — matching this codebase's own established
measurement convention (`07_stress`'s `--draws`, `09_scene`'s `--stress`
both measure the identical way) and RC7's "prefer offscreen/headless
measurement" preference. Used to produce this report's own frame-time
numbers below.

### 3. Packaging (`tools/package_samples.sh`) — three real gaps closed

All three were found by this task's own standalone-unzipped-copy
verification (unzip outside the build tree, run each binary fresh) — none
were caught before because CI's own packaging step and `ctest` run both
only ever exercise the **build tree**, where CMake's own `POST_BUILD`/
custom-command deploy steps had already staged everything. None are
regressions from this round's own code changes; all are inherited defects
from Tasks 7/8/10, closed in-round per the no-deferred-fixes standing rule.

| # | Gap | Root cause | Symptom (standalone, before fix) | Fix |
|---|---|---|---|---|
| 1 | `06_materials`/`08_gltf_viewer`/`09_scene` `material_shaders/` missing `brdf.slang`/`energy_compensation_{off,on}.slang` | `MaterialSystem::create()` has required these three **unconditionally** in `sharedShaderDir` since Task 8 (#44) landed — each sample's own `CMakeLists.txt` already deploys them into its build tree, but `package_samples.sh`'s `copy_required` lists were never updated to match | `MaterialSystem::create failed` (06/08/09 all fail before rendering a single frame) | Added the 3 files to all 3 samples' `copy_required` calls |
| 2 | `08_gltf_viewer` missing `ibl_shaders/`+`environments/` | `rx::ibl::bakeEnvironment()`'s explicit `shaderDir` lookup + `resolveDefaultEnvironmentPath()`'s packaged-first lookup both existed since Task 10 (#46); never staged by this script | `--env` default silently degrades to "no environment bound" (non-fatal, so this one degrades quietly rather than crashing — the Stage 1 demonstrator's own headline capability missing from every redistributed copy) | Staged both directories (5 ibl shaders incl. `skybox.slang` + the committed `.hdr` fixture) |
| 3 | `09_scene` missing `ibl_shaders/`+`environments/` | Same root cause as #2 — `09_scene`'s own `resolveEnvironmentPath()` and `CMakeLists.txt` build-tree deploy step both existed since Task 10 too | Same silent zero-indirect-lighting degradation | Staged both directories (4 ibl shaders, no `skybox.slang` — 09 renders no skybox pass) |

Before/after evidence (real, not simulated) is in the Verification section.

Header-comment manifest (lines ~30-125 of `package_samples.sh`) and
`samples/README.md`'s directory-tree listing were both updated to match.

### 4. CI (`.github/workflows/ci.yml`)

No new CI *mechanism* — the packaging step's own explanatory comment block
was expanded to name all three fixes above (so a future reader of a CI
diff understands why the manifest grew). No workflow-logic changes were
needed: the existing `ctest`/packaging/upload steps already do the right
thing once the script itself is correct.

### 5. Docs (`README.md`, `samples/README.md`, `MANUAL_VERIFICATION.md`)

- `README.md`: `08_gltf_viewer` bullet updated (BRDF/IBL/exposure/HUD);
  the stale "Phase 5 and beyond" roadmap line replaced with an accurate
  "Phase 5 (in progress) — Stage 1 complete" summary of T7-T12's delivered
  engine growth.
- `samples/README.md`: the `08_gltf_viewer` section was substantially
  stale (still described Phase-4-era flat ambient, "full IBL is a
  techniques-phase concern, not built here" — wrong since Task 10) — fixed
  as a discovered pre-existing doc defect, not merely appended to. Added
  `--env`/`--no-env`/`--env-intensity`/`--bench-frames` flag docs, HUD
  description, updated Redistribution section (ibl_shaders/environments +
  the packaging-fix note), directory-tree listing. Also added a matching
  fix-note to `06_materials`'s own Redistribution section (the sibling
  packaging gap).
  - **Known pre-existing gap, NOT fixed this round**: `samples/README.md`
    has **no per-sample section for `09_scene` at all** (the file jumps
    from `## 08_gltf_viewer` straight to `## Building and running`) — a
    structural absence from whenever 09 was added, out of this ticket's
    scope to backfill (writing a full new per-sample walkthrough section
    is a substantially larger, unscoped undertaking than this ticket's
    "README rows updated" line calls for). Flagged here rather than
    silently left undiscovered.
- `MANUAL_VERIFICATION.md`: `08_gltf_viewer` section rewritten — HUD/`--env`
  content added to "what pass means"; checkboxes now reflect exactly what
  this round verified (screenshot-confirmed default + `--exposure 5
  --env-intensity 2.0` runs) versus what it explicitly did **not** exercise
  in `--present` mode this round (mouse-drag orbit feel, `--scene`, `--env
  <other path>`, `--no-env` — all covered by the existing headless suite,
  just not by a human/screenshot check in present mode); "Last run" note
  rewritten with the real Xephyr/NVIDIA session's actual findings, not a
  restated placeholder.

## Task 5 audit row (per the ticket's own acceptance line)

Scoped to exactly this ticket's new surface area (env/exposure/HUD), per
the matrix's own framing — not a re-audit of the whole sample.

| Surface | Consumes (engine API) | Sample-local code | Disposition |
|---|---|---|---|
| Environment binding | `rx::ibl::bakeEnvironment()`, `rx::scene::Scene::setEnvironment()` | `App::environmentPath` (plain `std::string`, display-only bookkeeping) | Zero-tolerance clean — unchanged from Task 10, no reimplementation |
| Exposure | `rx::scene::Camera::setExposure()`/`ev100()`/`exposure()` | `App::exposureOverrideFromCli` (plain `bool`, display-only CLI-provenance flag — see the HUD bug above for why it's needed) | Zero-tolerance clean — unchanged from Task 4, no reimplementation |
| HUD rendering/input | `rx::debug_ui::Overlay` (create/processEvent/beginFrame/addPass) + `ImGui::*` | `drawHud()` — WHAT text to show (content/layout), never HOW to render it | Matches `09_scene`'s own established architecture exactly: the engine owns the GPU-facing render-graph pass, the sample owns its own (inherently sample-specific) readout content. Not a new pattern needing promotion. |
| Frame-time benchmark (`--bench-frames`) | `rx::graph::Executor::execute()`, the pre-existing `captureFrame()` closure, `std::chrono` | The whole loop (orchestration only) | Same sample-local-benchmark-loop precedent as `07_stress`'s `--draws`/`09_scene`'s `--stress` (neither promoted to an engine facility either) — consistent, not a gap |

Zero undissposed rows.

```
$ grep -n "ImGui::\|rx::debug_ui::Overlay" samples/08_gltf_viewer/main.cpp | wc -l
19
$ grep -n "FT_\|stb_truetype\|freetype" samples/08_gltf_viewer/main.cpp
(no output)
```

## Discrimination proofs

**1. HUD render-graph pass (new gate surface).** The two D17-gated frames
draw NO widgets and stay byte-identical to their committed references
(0/65536 failing pixels, both drivers, no regen needed — see Verification);
the separate HUD smoke-test frame DOES draw real widgets and asserts
`ImGui::GetDrawData()->CmdListsCount > 0 && TotalVtxCount > 0`. This
dichotomy is the load-bearing proof that the new pass actually renders
content when content is issued, and does NOT corrupt output when it isn't
— exercised every `ctest` run (`sample_08_gltf_viewer_headless`).

**2. Packaging fix — real before/after, not simulated.** Captured directly:

```
# BEFORE (zip built from the pre-fix package_samples.sh, unzipped fresh):
$ ./sample_08_gltf_viewer --validate
[error] rx_material: could not read shared shader file '.../material_shaders/energy_compensation_off.slang'
[error] sample_08_gltf_viewer: MaterialSystem::create failed
exit=1

$ ./sample_09_scene --validate   # after fix #1 alone, before fix #3
[error] rx_shader: error: could not open shader file '.../ibl_shaders/equirect_to_cubemap.slang'
[error] sample_09_scene: rx::ibl::bakeEnvironment failed for '/media/ywadi/.../08_gltf_viewer/environments/gate_test_env.hdr'
[error] sample_09_scene: D17 grid_scene gate FAILED on lavapipe
exit=1

# AFTER (zip built from the fixed package_samples.sh, unzipped fresh, all nine samples):
01_triangle: exit=0   02_hotreload: exit=0 gate PASSED   03_bindless_mesh: exit=0 gate PASSED
04_streaming: exit=0 gate PASSED   05_multipass: exit=0 gate PASSED   06_materials: exit=0 gate PASSED
07_stress: exit=0 gate PASSED   08_gltf_viewer: exit=0 gate PASSED   09_scene: exit=0 gate PASSED
```

**3. Exposure-label HUD bug — real before/after screenshots.** See
Verification below (screenshots 2 vs. 3/4): before the fix, the default
(no `--exposure` passed) run's HUD read "(direct EV100 override engaged,
--exposure)"; after the fix, the same default run reads "(neutral
default)", and only `--exposure 5` shows the override annotation.

## Verification

### Both presets, both drivers, Wine

| Suite | Driver | Result |
|---|---|---|
| Full `ctest` (42 tests), `linux-native` | lavapipe (llvmpipe, Mesa 25.1.5, LLVM 15.0.7) | 42/42 passed, twice (once before the exposure-label fix rebuild, once after) |
| `sample_08_gltf_viewer_{headless,quit_during_load,exr_env_headless}` | NVIDIA GeForce RTX 2080, driver 580.82.07 | 3/3 passed, twice (same two rounds) |
| `ctest -E '...\|sample'` (14 tests, CI's own Wine-exclusion regex), `windows-cross-zig` | Wine (CPU-only tier — samples are excluded from Wine by the SAME regex CI itself uses; unaffected by this round) | 14/14 passed |
| `windows-cross-zig` build | N/A (compile only) | Clean build, all 257 targets, zero errors |
| `tools/check_byte_source_invariant.sh` (RC7e) | N/A | OK, unaffected |

Zero unfiltered Vulkan validation errors in any run (`--validate` on
throughout; only this codebase's own documented false-positive guards
matched, e.g. the separate-sampler `SYNC-HAZARD-READ_AFTER_WRITE`
misclassification and the pre-`VK_KHR_portability_enumeration`/pre-Slang-
SourceLanguage layer quirks — both pre-existing, both named in-source).

### Present-mode / HUD real-display verification

Nested Xephyr (`:77`, 1280x720) on the real NVIDIA GeForce RTX 2080 (driver
580.82.07) — serialized, `nice -n 10`, bounded windows, no interference
with the owner's real desktop. Screenshots saved under this session's
scratchpad (not committed — throwaway verification artifacts):

- `sample08_present_screenshot2.png` — **first draft**, default args:
  render shows real IBL + skybox; HUD shows the exposure-label bug
  ("direct EV100 override engaged" despite no `--exposure` passed).
- `sample08_present_screenshot3_default.png` — **after the fix**, default
  args: HUD correctly reads "(neutral default)"; `ev100: 14.966`,
  `pre-exposure multiplier: 1.000000`.
- `sample08_present_screenshot4_exposure5.png` — **after the fix**,
  `--exposure 5 --env-intensity 2.0`: both skybox and helmet visibly
  darker (correct EV100 direction); HUD reads "(direct EV100 override
  engaged, --exposure)", intensity `2.000`.

All three runs closed cleanly (`window closed cleanly`), zero unfiltered
validation errors. **Scope note** (recorded honestly, not silently
assumed): mouse-drag orbit feel, `--scene`, `--env <other path>`, and
`--no-env` were NOT re-exercised interactively in `--present` mode this
round — only default-args and the exposure/intensity combination above
were screenshot-verified. The other three flags are covered functionally
by the existing headless suite (default-env path, the EXR routing test,
T10's own no-env-vs-env discrimination captures) but not by a
human/screenshot check in present mode specifically this round.
`MANUAL_VERIFICATION.md`'s own checkbox list reflects this distinction
exactly (checked vs. unchecked rows).

### Packaged zip standalone verification

Built via `tools/package_samples.sh linux-native linux-x86_64 <zip>` and
`... windows-cross-zig windows-x86_64 <zip>`; the Linux zip was unzipped to
a directory **outside the build tree entirely**
(`/tmp/.../scratchpad/standalone_test3/`) and every one of the nine sample
binaries run fresh from there:

```
01_triangle: exit=0
02_hotreload: exit=0 status=gate PASSED
03_bindless_mesh: exit=0 status=gate PASSED
04_streaming: exit=0 status=gate PASSED
05_multipass: exit=0 status=gate PASSED
06_materials: exit=0 status=gate PASSED
07_stress: exit=0 status=gate PASSED
08_gltf_viewer: exit=0 status=gate PASSED
09_scene: exit=0 status=gate PASSED
```

Repeated against the NVIDIA driver for the three samples this round's own
fixes touched (06/08/09): all `exit=0`, `gate PASSED`, zero unfiltered
validation errors. `09_scene`'s own standalone log confirms the environment
now bakes from the packaged copy, not a source-tree fallback:

```
sample09: perf ibl_bake path='/tmp/.../standalone_test3/09_scene/environments/gate_test_env.hdr' ...
```

**[Updated, Fix round 1]** The original submission scoped the Windows zip
check down to a structural `unzip -l` listing only, judging a live Wine
run "disproportionate." The independent review round went further and
actually ran it: built the Windows zip, unzipped it, and ran
`sample_08_gltf_viewer.exe` live under Wine (offscreen Xvfb, no
on-desktop window) — it passed cleanly, environment baked and bound from
the packaged copy's own path, `headless gate PASSED` (the one pixel-gate
"fail" line is Wine/wined3d's own informational-only non-lavapipe note,
matching this codebase's established non-enforcement convention off the
reference driver). The review's own adjudication: the scope-down was a
defensible call given the cost/benefit at the time, but the check was
cheap enough (under two minutes, zero setup beyond what the Wine CI tier
already requires) that it should just be routine for this class of
packaging fix going forward. Citing the review's own verified result here
rather than re-running it a third time — the Windows packaging fix is now
**live-Wine-verified**, not merely structurally checked.

## Numbers — Stage 1 checkpoint (driver-labeled) [SUPERSEDED — see "Fix
round 1" below]

**This section's own frame-time table is SUPERSEDED.** An independent
review found the `--bench-frames` methodology conflated render cost with
GPU-readback machinery cost (contradicting this report's own since-
corrected claim of parity with `07_stress`/`09_scene`'s convention), and
found the lavapipe figures specifically did not reproduce in a quiet
environment (helmet ~150% high, Sponza ~70% high — a real, undisclosed
host-contention artifact from this measurement session, not noise). Left
in place below for the historical record of what shipped in the original
commit; **use the "Fix round 1" section's own corrected table for the
real Stage 1 baseline.** The bake-timing table immediately below is
UNCHANGED and still current (bake timing was never in question).

**Hardware**: Intel Core i7-9700F (8 cores) host. GPU drivers: NVIDIA
GeForce RTX 2080 @ 580.82.07; lavapipe (llvmpipe, Mesa 25.1.5, LLVM 15.0.7,
software rasterizer). Steam Deck rows: not run — no Deck hardware in this
loop yet (RC8/honest-manual posture, matching every prior Phase 5 round's
own Steam Deck section).

**Methodology**: all numbers below via `--bench-frames` (offscreen,
headless, CPU-record + GPU-submit-and-wait wall time per iteration — see
this report's own "What shipped" section #2 for why this is NOT vsync-paced
present-mode timing). Bake timings are `rx::ibl::BakeTimings`, wall-clock
around each real one-shot GPU submission (`rx_ibl/bake.h`'s own documented
convention, inherited unchanged from Task 9). The SAME committed
`environments/gate_test_env.hdr` fixture (64x32 procedural HDR) is baked
once per process for every scene below — bake cost is a property of the
environment, not the loaded glTF scene, so these numbers cross-check Task
9's own already-published figures rather than re-measuring something new.
Frame-bench sample counts: 60 iterations/scene on lavapipe (bounded for
honest wall-clock cost under software rasterization), 200 iterations/scene
on NVIDIA (cheap enough to afford more samples).

### IBL bake timings (equirect→cubemap → irradiance → prefiltered specular → DFG LUT)

| Stage | lavapipe (ms) | NVIDIA RTX 2080 (ms) |
|---|---|---|
| equirect→cubemap | 3.5 – 7.8 | 0.55 – 0.56 |
| irradiance convolve | 5.2 – 26.3 | 0.39 – 0.48 |
| prefilter specular | 8.4 – 72.9 | 1.25 – 1.29 |
| DFG LUT | 6.0 – 18.8 | 0.24 – 0.29 |
| **total** | **219 – 517** | **219 – 225** |

(Ranges across the 3 helmet/Sponza/Workshop runs — the bake itself is
scene-independent; the spread is host-load/scheduling noise, not a signal.
Consistent with Task 9's own published <4ms-per-stage-on-real-driver
figure for the individual GPU-side stages; the ~220ms "total" floor on
BOTH drivers is Task 9's own documented Slang-compile-derived-data-cache
cost, not bake work itself.)

### Frame times (helmet / Sponza / Workshop, full Stage 1 pipeline — real IBL + skybox + Task 7/8 BRDF, not the Phase-4 flat-ambient path) — SUPERSEDED, see "Fix round 1"

| Scene | Driver | avg ms | min ms | max ms | Import (ms) | First-frame (ms) |
|---|---|---|---|---|---|---|
| DamagedHelmet (default) | lavapipe | 11.799 | 6.691 | 30.188 | 773 | 2890 |
| DamagedHelmet (default) | NVIDIA RTX 2080 | 1.703 | 1.566 | 2.359 | 563 | 1638 |
| Sponza | lavapipe | 52.134 | 30.115 | 345.496 | 8211 | 10645 |
| Sponza | NVIDIA RTX 2080 | 7.479 | 6.920 | 11.300 | 3832 | 4902 |
| Workshop (531k tris, 4K textures — LOCAL benchmark only, RC8's own caveat: not scriptably fetchable, CI never has a copy) | lavapipe | 116.589 | 107.604 | 260.880 | 6649 | 7696 |
| Workshop | NVIDIA RTX 2080 | 12.332 | 11.407 | 16.351 | 6429 | 7505 |

Import/first-frame times are the SAME "sample08: perf scene=..." line this
sample has published since the Phase 4 fix round (unaffected methodology,
included here for the checkpoint's own completeness, not re-derived).

Raw log lines (both drivers, all three scenes) are preserved in this
session's own scratchpad and quoted verbatim in the Discrimination proofs/
Verification sections above where relevant; full command tails available
on request.

## Files changed

- `samples/08_gltf_viewer/main.cpp` — HUD (Overlay wiring, `drawHud()`,
  `App::overlay`/`environmentPath`/`exposureOverrideFromCli`),
  `--bench-frames` flag + headless benchmark loop, `#include <rx_debug_ui/
  overlay.h>`/`<imgui.h>`/`<numeric>`.
- `samples/08_gltf_viewer/CMakeLists.txt` — link `rx_debug_ui`.
- `tools/package_samples.sh` — 3 packaging-gap fixes (06/08/09 material
  shaders; 08/09 ibl_shaders+environments) + header-comment manifest
  updates.
- `.github/workflows/ci.yml` — packaging-step comment expanded to name the
  3 fixes; no workflow-logic change.
- `README.md`, `samples/README.md`, `MANUAL_VERIFICATION.md` — see "What
  shipped" #5 above.

## Ambiguities / decisions taken (best-recommended option, per dispatch instructions)

1. **Matrix's "third reference image" row** — judged N/A. The matrix wrote
   this before Task 10 landed; `loaded_scene.png` was already regenerated
   during Task 10's own fix round to reflect the environment-bound default
   state (`git log` confirms), so the D17 gate already exercises the
   env-driven render — no new reference image needed. Recorded, not
   silently assumed (confirmed via `git log` on the reference PNG before
   concluding this).
2. **CI perf-regression-gate mechanism** — NOT built this round. RC8's own
   text assigns this to T36 explicitly ("T36 BUILDS the CI perf-regression
   gate mechanism"); T12's own obligation is the published-numbers
   discipline only, which this report satisfies.
3. **Windows-standalone Wine run** — originally scoped down to a structural
   zip-content check. **[Fix round 1]** The independent review round
   overrode this scope-down (judged it worth doing, not load-bearing
   either way) and ran the unzipped Windows binary live under Wine itself
   — passed cleanly. See the Verification section's own updated packaging
   row, which now cites the review's real run instead of the structural
   check.
4. **`samples/README.md`'s missing `09_scene` section** — originally
   flagged, not fixed, on the call that it was out of this ticket's scope.
   **[Fix round 1]** The coordinator's review-round ruling overrode that
   self-assessment: closed, along with the same-class `07_stress` gap the
   review additionally found. See the Fix round 1 section below.

None of the above are load-bearing enough to warrant NEEDS_CONTEXT — all
had a clear best-recommended resolution (superseded by the coordinator's
own review-round rulings where noted above).

## Fix round 1 (independent review response)

Review verdict on commit `365a187`: spec NOT clean, quality NOT Approved
(2 MAJOR, 1 MINOR) — full review at `task-12-review.md`. All three close
in-round below.

### Finding 1 (MAJOR) fixed — `--bench-frames` measured window corrected

The review's own instrumented-build finding was real: `captureFrame()`'s
wall-clock (the previous published metric) is dominated by GPU-readback
machinery, not render cost — 88% of the published number for the default
DamagedHelmet scene on NVIDIA. The report's claim that this "matches"
`07_stress`/`09_scene`'s convention was factually wrong; those two samples
deliberately time ONLY `executor->execute()` inside the record callback,
explicitly excluding the submit-and-wait and any readback.

**Fix**: `captureFrame()` (`samples/08_gltf_viewer/main.cpp`) now takes an
optional `double* recordMsOut` parameter, timed around ONLY
`executor->execute()` inside its first `runOnce()` call — byte-for-byte
the same pattern `samples/09_scene/main.cpp`'s own `captureFrame()` already
uses (verified by direct comparison, not just structural similarity). The
`--bench-frames` loop now publishes **two distinct, separately-labeled**
metrics per scene, from the same capture (never rendered twice):

- `cpu_record_{avg,min,p95,max}_ms` — the new HEADLINE metric, directly
  comparable to `07_stress`'s/`09_scene`'s own `cpu_record_ms`.
- `full_{avg,min,p95,max}_ms` — the previous whole-call wall-clock
  (record + submit-and-wait + a second submit-and-wait for the
  `vkCmdCopyImageToBuffer` readback + a freshly-allocated host-visible
  buffer + `invalidate()` + `memcpy`), kept as an explicitly-separate,
  explicitly-labeled secondary figure — a real cost this sample's own
  headless capture path pays, never presented as "frame time" unqualified
  again.

p95 is nearest-rank (`ceil(0.95*N)`, 1-based, clamped) on a sorted copy of
the per-iteration samples. `Args::benchFrames`' own header comment documents
the exact measured window for both metrics (the reviewer's own "document
the measured window precisely... in the flag's help text" instruction —
this sample has no `--help` text at all, so the flag's own doc comment,
the closest equivalent, carries this).

### Finding 2 (MAJOR) fixed — re-measured on a quiet host, numbers reproduce

Host state confirmed quiet before measuring (not niced — the reviewer's
own instruction: "run the measured process itself at normal priority and
say so"): `uptime` load average 0.89-1.19 (8 cores), `nvidia-smi
--query-compute-apps` empty, `ps aux --sort=-%cpu` showed no concurrent
builds/heavy processes. **Root cause of the original discrepancy,
confirmed**: the original lavapipe measurement session ran concurrently
with this same round's own background `windows-cross-zig` build and Wine
`ctest` run (both CPU-heavy) — undisclosed at the time, exactly as the
review inferred from the original table's own outlier max-spikes. The
corrected, quiet-host numbers below reproduce the review's own independent
figures closely (helmet lavapipe: 4.60ms vs. reviewer's 4.667-4.717ms;
Sponza lavapipe: 30.46ms vs. 30.575ms; Workshop lavapipe: 107.24ms vs.
107.424ms — all within ~2%; the NVIDIA Workshop figure that previously
diverged ~24-26% now reads 15.79ms, matching the reviewer's own
15.25-15.50ms range within ~2-3.5%).

**Measurement conditions** (both drivers): quiet host as described above;
measured process run WITHOUT `nice` (normal scheduling priority); same
default `environments/gate_test_env.hdr` fixture; same `--bench-frames`
counts as the original round (60 iterations/scene lavapipe, 200/scene
NVIDIA — kept for direct before/after comparability); `sample_08_gltf_viewer`
rebuilt from the Finding-1 fix immediately before measuring, `ctest`
re-confirmed green on both drivers first (3/3 each).

### Corrected numbers — Stage 1 checkpoint frame times (headline metric: `cpu_record_ms`, directly comparable to `07_stress`/`09_scene`)

| Scene | Driver | cpu_record avg (ms) | min | p95 | max | full avg (ms, render+readback round-trip) | min | p95 | max |
|---|---|---|---|---|---|---|---|---|---|
| DamagedHelmet | lavapipe | 0.253 | 0.213 | 0.296 | 0.333 | 4.598 | 4.322 | 4.784 | 4.834 |
| DamagedHelmet | NVIDIA RTX 2080 | 0.219 | 0.196 | 0.269 | 0.304 | 1.715 | 1.625 | 1.829 | 3.386 |
| Sponza | lavapipe | 4.644 | 4.391 | 5.086 | 5.170 | 30.463 | 29.629 | 32.093 | 32.742 |
| Sponza | NVIDIA RTX 2080 | 4.547 | 4.366 | 4.657 | 5.086 | 7.197 | 6.418 | 7.738 | 11.380 |
| Workshop (local-only, RC8) | lavapipe | 9.760 | 9.081 | 10.752 | 11.563 | 107.244 | 104.083 | 112.226 | 114.896 |
| Workshop (local-only, RC8) | NVIDIA RTX 2080 | 9.875 | 9.296 | 10.285 | 11.955 | 15.790 | 14.530 | 16.590 | 26.534 |

Both metrics come from `--bench-frames`' own `sample08: perf frame_bench`
log line; raw lines preserved in this session's own scratchpad. Bake
timings (previous table, unchanged) remain valid — bake methodology was
never in question.

### Finding 3 (MINOR) fixed — `samples/README.md` `09_scene`/`07_stress` sections added

Added concise `## 07_stress` and `## 09_scene` sections (What/CLI flags/
expected output/Redistribution, matching `06_materials`'/`08_gltf_viewer`'s
own format) between `## 06_materials` and `## 08_gltf_viewer`, and between
`## 08_gltf_viewer` and `## Building and running`, respectively. Also
extended the top-of-file directory-tree listing (the "Downloading a
prebuilt sample bundle" section) with `09_scene`'s own entry (previously
absent there too) and `06_materials`'/`08_gltf_viewer`'s tree entries with
the `brdf.slang`/`energy_compensation_{off,on}.slang` files their own
Redistribution sections already documented in prose; fixed a stale "the
other seven" count to "the other eight" (off-by-one predating `09_scene`'s
own addition to this file, found while making this same edit).

### Windows-zip packaging row — updated to cite the review's own live-Wine run

See this report's own updated Verification section above (the "Windows
zip's structure was verified..." paragraph was replaced) — the independent
review went beyond this report's original structural-only check and ran
the unzipped Windows binary live under Wine; it passed cleanly. Not
re-run a third time by this fix round; cited directly.

### Re-verification after the fix round

- `cmake --build --preset linux-native` — clean rebuild, zero errors.
- `ctest --test-dir build/linux-native -R sample_08_gltf_viewer` — 3/3
  passed, BOTH drivers (lavapipe and NVIDIA RTX 2080, re-run separately).
- Smoke-checked the new log line shape (`--bench-frames 10`) before
  committing to the full 6-scene×2-driver re-measurement pass.

## Commit(s)

Round 1: `365a187` (original submission). Fix round 1: see final message
for the new commit hash on `task/t12-stage1-exit`. Not pushed; main
untouched; author = local git config; no AI attribution.
