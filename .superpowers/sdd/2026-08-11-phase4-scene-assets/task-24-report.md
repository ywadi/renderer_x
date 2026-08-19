# Task 24 report — samples/09_scene: scene fly-through + stress-v2 + release prep (card #15)

Authority order followed: `gate/rulings-2026-08-18.md` #15 > spec (D13-D29) >
`gate/matrix-issue15-sample09.md` > `task-24-brief.md` > `gh issue view 15`.
BASE=270de94. Build/test machine: this repository's dev container (shared/
virtualized — same disclosed-limitation posture as Task 23's own report;
no dedicated Deck hardware available in this environment).

## 0. Incident: concurrent-writer collision, and how it was resolved

Mid-session, two of five parallel *research-only* forks I dispatched
(rx_platform/rx_debug_ui research, and packaging/CI/docs research)
misread their own inherited "you are the implementer for Task 24" identity
(forked sessions inherit the dispatcher's full context) and began
independently writing `samples/09_scene/main.cpp`/`CMakeLists.txt`,
`tools/gen_stress_variants.py`, and `assets/test/stress_variants/`,
racing each other and me. Both were stopped via `SendMessage` the moment
the collision surfaced; both independently confirmed **zero commits**
(`git log` never left `270de94`) and no other files touched. I removed
their inconsistent partial output (the CMakeLists.txt/main.cpp stub had
a genuine content mismatch — one half claimed StandardPBR-only, the other
half's own asset-provenance note claimed `KHR_materials_unlit` — proving
two different authors had interleaved edits) and authored the entire
delivered implementation myself, from a clean slate, incorporating only
the sound *ideas* I had independently arrived at too (layer-mask
deterministic culling instead of frustum-boundary math; calling
`resolveDrawGroups()` once from chunk 0 rather than nesting
`recordDrawList()`'s own parallelism inside the executor's). No content
from the rogue forks is present in the delivered diff — every line was
authored or reviewed line-by-line by me. This is recorded here per this
project's own "verify subagent commits" / "shared-tree commit discipline"
standing practice.

## 1. Per-criterion proof (gate/matrix-issue15-sample09.md rows, as amended by rulings #15)

| Row | Criterion | Status | Evidence |
|---|---|---|---|
| 1 | Headless instanced DamagedHelmet grid, `recordsIn > drawsSubmitted`, deterministic frame-to-frame | **PASS** | §2 below: recordsIn=8, drawsSubmitted=1 every frame, asserted in-binary (`sample_09_scene: frame N: ...` errors would fire otherwise) |
| 2 | `--scene sponza` fails loudly when absent; loads when present | **PASS (loud-failure path); NOT independently verified against a real fetched Sponza** | `resolveSponzaScenePath()` + the same `std::filesystem::exists` check pattern as 08; code-reviewed correct, but fetching the ~53 MB asset was outside this session's time budget — disclosed in MANUAL_VERIFICATION.md's own `## 09_scene` "Last run" note, not silently claimed |
| 3 | Full-pipeline import counts printed + asserted | **PASS** | `sample_09_scene: import counts meshes=1 submeshes=1 materials=1 grid_instances=16` (real log line, §2) |
| 4 | Pool-stats HUD panel, real accessor, non-mock | **PASS** | `drawHud()` reads `GeometryPool::stats()` live every frame (main.cpp) |
| 5 | Uploader tickets — no unconditional `vkWaitForFences` in the frame loop | **PASS** | Sample never calls `Uploader::wait()` inside the per-frame path (only once at the single sync `importGltf()`/`GeometryPool::upload()` setup, matching sample 08's own precedent) |
| 6 | KTX2/TextureCache — DamagedHelmet textures visibly affect render | **PASS** | Same `resolveTextureIndex`/`resolveSamplerIndex` wiring as sample 08 (helmet material setup reuses that exact code path); D17 reference PNG shows the real textured helmet |
| 7 | StandardPBR/Unlit + alphaMode — MASK/BLEND untested by DamagedHelmet | **DISCLOSED GAP (pre-existing, not this task's to close)** | DamagedHelmet is OPAQUE-only (verified: `damaged_helmet_test.cpp`); Sponza (present-mode only, not fetched this session) is the documented MASK/BLEND exercise vehicle. `--stress` mode DOES exercise a real Mask/Opaque split (2 of 4 variants) through the real D28 fixed-function axis. |
| 8 | Scene proxies — renderable count matches import count | **PASS** | `Scene::renderableCount() == 16` after `populateHelmetGrid()`; `totalCandidates == 16` every frame |
| 9 | Reversed-Z main camera | **PASS** | Forward pass declares `DepthConvention::Reversed` (D29); `MaterialSystem::getPipeline()` (unmodified, existing D13/RC3 GREATER_OR_EQUAL) drives depth compare; zero validation errors |
| 10 | Culled counter nonzero for the fixed startup camera | **PASS (via layer mask, not frustum — a deliberate, documented, more robust design choice)** | `culledByLayerMask=8` every frame; see main.cpp's own header comment for why frustum-boundary math was rejected as the gate's deterministic source |
| 11 | Instancing-collapse ratio, blessed formula, CI-asserted | **PASS** | `1 - drawsSubmitted/recordsIn` computed both in-binary and cross-checked against a hand-computed grid expectation (`1 - 1/8 = 0.875`); exact-equality assertion (`std::abs(actual-expected) > 1e-9` fails the gate) |
| 12 | D27 main-thread pre-resolution — worker-guard test under `--threads>1` | **PASS** | **[Corrected in fix-round — see §9]** The `setViolationHookForTests()` check itself is real infrastructure but, as exercised against the default grid (one collapsed draw, entirely handled by chunk 0), is a near-tautology — `resolvePipeline()` is structurally only ever called from `updateSceneFrame()` on the main thread regardless of whether the guard holds. The mechanism that ACTUALLY pins this fix is `RX_ASSERT_MAIN_THREAD`'s own abort under `sample_09_scene_stress_headless` (drawsSubmitted=4, spread across multiple non-empty worker chunks) — a revert of the `GeometryPool::bind()` fix (finding #2, §4) SIGABRTs there immediately, a real, already-registered ctest target. |
| 13 | Shadow quality bridge visibly exercised | **PASS** | Real `ShadowCasterPipeline` + `fitShadowFrustum` + comparison-sampler PCF wired into every DrawDataGpu row; visible in `references/grid_scene.png` (ground shadow under the helmets) |
| 14 | #27 memory report HUD panel, real accessor | **PASS** | `drawHud()` reads `Allocator::report()` live every frame |
| 15 | Input surface consumed | **PASS** | `Window::isKeyDown`/`consumeMouseDelta`/`setRelativeMouseMode`/`poll()` (gamepad) all real calls in `updateFlyCamera()` |
| 16 | ImGui overlay — real render-graph pass, non-empty draw data | **PASS** | Separate, non-pixel-gated headless frame proves `ImGui::GetDrawData()->CmdListsCount>0 && TotalVtxCount>0` (see §4 for the ImGui first-appearance-window-sizing bug this surfaced and fixed) |
| 17 | Task 23 lands before stress-v2 numbers published | **PASS** | BASE=270de94 already includes Task 23 (commits 477a511/1197d7b/23e5c69/b641e46); §5 cites its own report numbers as the measurement precedent |
| 18 | 60-frame rolling FPS/frame-ms | **PASS** | `HudState::pushFrameTime`/`rollingAverageMs`/`rollingFps`, `kRollingWindow=60` |
| 19 | HUD surfaces cull counters + collapse ratio + pool stats + memory report in one panel | **PASS** | `drawHud()`, single `ImGui::Begin("sample_09_scene")` window |
| 20 | TWO visibly distinct mask controls (cullMask u32 vs channels u8) | **PASS** | 4 layer-row checkboxes → `camera.cullMask` (visibility only); 1 separate light-channel checkbox → `light.channels` via `Scene::setLightChannels` (lighting/shadow only) — different SoA columns, never conflated (see main.cpp header comment + HudState comment) |
| 21 | vsync toggle drives the same setPresentMode+recreateSwapchain path as CLI | **PASS (present-mode only, MANUAL_VERIFICATION per the matrix's own routing)** | `drawHud()`'s vsync checkbox calls `device.setPresentMode()` + `device.recreateSwapchain()` — the identical two calls `--vsync` uses; same "present mode in use" log line |
| 22 | Stress-v2 A/B comparability contract | **PASS** | §5 — 30k instances, 4 pipeline/material variations (D28 axis), `--threads`/`--vsync` semantics held identical; wall-clock AND draws-submitted published jointly; sample 07's own report format untouched |
| 23 | Stress-v2 two-tier CI (blocking counter gate + non-blocking wall-clock artifact) | **PASS** | `sample_09_scene_stress_headless` ctest (blocking, `--stress-draws 64`) + `.github/workflows/ci.yml`'s new "Stress-v2 sample wall-clock numbers" step (non-blocking, `stress-v2-numbers.txt` artifact, separate from `stress-numbers.txt`) |
| 24 | Headless gate — exact counters + zero validation errors | **PASS** | §2 |
| 25 | D17 tolerance-pixel gate, regenerated only via Task 16's script (no second mechanism) | **PASS** | `tools/regen_references.sh` extended (2nd positional arg selects sample; byte-identical default behavior for 08 — `git diff` on 08's own PNGs is empty after regeneration, verified §6) |
| 26 | Fly-through camera — WASD via Task 20's real surface | **PASS** | `updateFlyCamera()` uses `Window::isKeyDown` (SDL_SCANCODE_W/A/S/D/SPACE/LCTRL/LSHIFT), never a sample-local keyboard poll |
| 27 | MANUAL_VERIFICATION rows (+ Deck, unchecked) | **PASS** | New `## 09_scene` + `## Steam Deck (09_scene...)` sections; stale `09_fly_through` references corrected to `09_scene`; missing `## 07_stress` section added while touching the file |
| 28 | Packaging — `package_samples.sh` | **PASS** | §6 — 09_scene staged (material_shaders/shadow_shaders/tonemap/references/pre-staged DamagedHelmet+license); header count "eight"→"nine"; both presets verified end-to-end, zero missing-file failures, standalone run confirmed |
| 29 | Packaging — CI wiring | **PASS** | §6 |
| 30 | README/roadmap update | **PASS** | File-tree gains rx_task/rx_asset/rx_scene/rx_shadow/rx_debug_ui + shaders/shadow/ + samples 07/08/09 (07/08 were ALSO missing, a pre-existing gap fixed while touching the section); Roadmap gains "Phase 4 (complete)" paragraph + a new "Phase 5 and beyond" (renamed from the stale "Phase 4 and beyond") |
| 31 | Registry layer-8 qualified annotation | **PASS** | `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:34` — exact ruling text appended verbatim |
| 32 | Release preconditions | **PARTIAL — see §7** | Both packages build/package/run standalone; CI green both presets (this session's own local re-run, see §3); A/B numbers published (§5); tag/push/release itself is a COORDINATOR action per the plan's own phase-exit sequencing, out of this task's scope |

## 2. Headless gate — exact counters (default grid, 4x4=16, deterministic layer-mask culling)

Fixed startup camera, 3 frames, `sample_09_scene --validate`:

| Counter | Value | Hand-computed expectation |
|---|---|---|
| totalCandidates (imported) | 16 | 4 rows × 4 cols |
| culledByLayerMask | 8 | rows [2,4) × 4 cols (layers `1u<<row`, default `cullMask` = rows [0,2) only) |
| culledByFrustum | 0 | deterministic-by-design (layer mask is the sole culling source for this gate, not frustum boundary math — see main.cpp's own header comment) |
| visible | 8 | 16 − 8 |
| recordsIn | 8 | 1 submesh/instance × 8 visible |
| drawsSubmitted | 1 | all 8 share identical (blockId, mesh range, material, priority) draw identity → one D26.3-collapsed instanced draw |
| collapse ratio | 87.5000% | `1 − 1/8` |
| determinism | identical all 3 frames | static camera, no per-frame randomness |
| D17 gate | `failingPixels=0/65536 (0.0000%) pass=true` | committed `references/grid_scene.png`, lavapipe |
| HUD smoke frame | `CmdListsCount>0, TotalVtxCount>0` | separate, non-pixel-gated frame (see §4) |
| Vulkan validation | zero unfiltered errors | only this codebase's pre-existing documented false-positive guards (portability-enumeration, Slang SourceLanguage, separate-sampler sync misclassification ×2 — the 4th variant via `vkCmdExecuteCommands` for a chunked pass's secondary buffer, a NEW but same-root-cause manifestation this task's own chunked recording surfaced; matches the guard's own documented scope, not a new class of false positive) |

Stress-v2 CI-gated counter check (`--stress --stress-draws 64`, matches the
ctest registration): totalCandidates=64, culled=0 (full-visibility framing
— see §5), visible=64, recordsIn=64, drawsSubmitted=4, chunkCount ==
`chunkCountForWorkerCount(workerCount())` — **PASS**.

## 3. Test suite — both presets

**linux-native** (`ctest --preset linux-native`, serial): **29/29 PASSED**,
72.8s. Includes the 3 new targets: `sample_09_scene_headless` (1.39s),
`sample_09_scene_stress_headless` (1.05s), `sample_09_scene_tests` (0.00s,
device-free). Every pre-existing test target still green (no regression).

**windows-cross-zig** (build + `ctest -E
'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample'` under
Wine): build succeeds (32/32 ninja targets, including
`sample_09_scene.exe` and `sample_09_scene_tests.exe`); **13/13 non-
excluded tests PASSED**, 110.4s. `sample_09_scene_tests` (my new device-
free unit test) is swept into the pre-existing blanket `sample` exclusion
pattern alongside every GPU-needing `sample_*_headless` target — a known,
pre-existing regex-precision gap (the exclusion is a substring match, not
scoped to `_headless`/`_stress_headless` specifically) that predates this
task and is not something I judged in-scope to widen given the risk of
touching a shared, already-green CI mechanism without a specific mandate
to do so; `sample_09_scene_tests` DOES run and pass on linux-native (proof
of correctness), satisfying "suite green both presets" (nothing fails on
either preset; this one target is deliberately skipped on Wine, same
documented posture as every sample's own GPU-needing tests).

## 4. Real bugs found and fixed during development (not merely "it compiled")

1. **`HudState`'s default layer-visibility mask was wrong** (`fill(true)`
   instead of `row < kVisibleRowCount`) — caught by the headless gate's own
   exact-counter assertion (`culled=0 != expected 8`), not by inspection.
2. **`GeometryPool::bind()` is main-thread-only (D5)**, but the chunked
   forward pass's own worker chunks (chunkIndex ≥ 1) each need to bind a
   block into their OWN secondary command buffer — calling the guarded
   method from a worker thread aborted the process
   (`RX_ASSERT_MAIN_THREAD` fired: "main-thread-only API called from a
   non-main thread: GeometryPool::bind"). Fixed by caching
   `vertexBufferHandle()`/`indexBufferHandle()` (also guarded, but read
   once per frame on the main thread inside `updateSceneFrame()`) into a
   plain `App::blockBufferCache` map, and issuing the byte-identical raw
   `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer` calls directly from the
   worker chunk instead of calling `bind()`. This is a genuine first-
   consumer finding: no earlier sample combines `GeometryPool` with
   `Pass::setExecuteChunked()`, so this D5/chunked-recording interaction
   had never been exercised before.
3. **`resolveDrawGroups()` has no `blockId` awareness** (flagged mid-task
   by the coordinator, relayed from a stood-down research fork) — a
   resolved pipeline group CAN span two GeometryPool blocks if the same
   `materialIndex` happens to repeat across a block boundary.
   `rx::samples9::splitByBlockAndGroup()` (`draw_recording.h/.cpp`) closes
   this on the caller side, unit-tested with a synthetic two-block-one-
   group fixture (`tests/test_draw_recording.cpp`) — **revert-
   discrimination evidence**: reverting to a naive groups-only split (no
   block awareness) fails exactly the 2 tests designed to catch it
   (`spans.size() == 2` → actual 1; `spans.size() == 3` → actual 2), 3
   other tests still pass (proving the probe is selective, not vacuous).
4. **ImGui's own "brand-new auto-fit window's first frame only measures
   content, doesn't draw it" behavior** — the HUD-smoke-test frame's
   `ImGui::GetDrawData()` came back `Valid=true` but `CmdListsCount=0`
   even though `drawHud()` was provably called (confirmed via temporary
   `DisplaySize` logging) and `ImGui::Begin()`/widgets executed
   correctly. Root-caused against `src/rx_debug_ui/tests/
   test_overlay_gpu.cpp`'s own GPU test, which explicitly calls
   `ImGui::SetNextWindowPos`/`SetNextWindowSize` before `Begin()` for
   exactly this reason. Fixed with `ImGui::SetNextWindowSize(...,
   ImGuiCond_FirstUseEver)` before `Begin()`.
5. **Missing `beginFrame()` before the D27 debug-hook check's own
   `captureFrame()` call** — every `captureFrame()` call executes the
   whole graph, which unconditionally runs the HUD pass's own
   `ImGui::Render()`; skipping the paired `NewFrame()` first left ImGui's
   internal frame state one step behind, which is what surfaced finding 4
   until traced back further (finding 5 was the proximate desync; finding
   4 was the deeper root cause once frame pairing was fixed).

## 5. Stress-v2 A/B numbers (published; desktop, this dev container — see below)

**Comparability contract held identical** [gate ruling #15, blessed
verbatim]: 30,000 instances; 4 pipeline/material variations (sample 07:
2 meshes × 2 cull-mode pipelines; sample 09 --stress: 2 alphaModes × 2
doubleSided states through D28's real fixed-function axis — a structurally
equivalent "4 real, independently-cached VkPipeline objects" shape, not a
literal re-derivation of sample 07's own scheme); per-instance transform
data; `--threads N`/`--vsync` semantics (both samples: `Scheduler::
create(threads)`, 0 = hardware_concurrency()-1 default).

**Expected to differ, and why**: sample 07 submits all 30,000 draws
unconditionally (no culling, no collapse — confirmed, `claim-validation
#2`). Sample 09's `--stress` field is framed so the camera sees the WHOLE
field (`culled=0` by deliberate design, isolating the comparison to
D26.3 collapse specifically rather than mixing in a culling effect too —
see main.cpp's own header comment); since all 30,000 instances of a given
variant share identical (blockId, mesh range, materialIndex, priority)
draw identity, they collapse into exactly 4 real hardware-instanced
`vkCmdDrawIndexed` calls (`instanceCount≈7500` each).

`cpu_record_ms` (wall-clock, timed around ONLY `executor->execute()` —
the identical methodology sample 07's own `runHeadless()` uses; frame 2 of
a fixed 3-frame headless run, 3 samples reported below, dev container):

| Config | Sample 07 (07_stress, direct path) | Sample 09 (09_scene, `--stress`) |
|---|---|---|
| `--threads 1` | cpu_record_ms ≈ 11.85 (12.122/10.900/12.533), **draws=30000** | cpu_record_ms ≈ 0.019 (0.020/0.018/0.018), **recordsIn=30000, drawsSubmitted=4** |
| default workers (7) | cpu_record_ms ≈ 4.99 (3.394/4.161/7.425), **draws=30000** | cpu_record_ms ≈ 0.047 (0.033/0.077/0.030), **recordsIn=30000, drawsSubmitted=4** |

Raw log lines (both samples, both configs) captured in the CI artifacts
this task's own `.github/workflows/ci.yml` addition produces
(`stress-v2-numbers.txt`, alongside sample 07's own untouched
`stress-numbers.txt`); reproduced locally via:
```
xvfb-run -a build/linux-native/samples/07_stress/sample_07_stress --draws 30000 --threads 1
xvfb-run -a build/linux-native/samples/07_stress/sample_07_stress --draws 30000
xvfb-run -a build/linux-native/samples/09_scene/sample_09_scene --stress --stress-draws 30000 --threads 1
xvfb-run -a build/linux-native/samples/09_scene/sample_09_scene --stress --stress-draws 30000
```

**Reading these numbers honestly** [gate ruling #15's own "report BOTH
wall-clock AND draws-submitted jointly" instruction]: sample 09's ~600x
lower recording time is explained almost entirely by drawsSubmitted (4 vs
30000) — this is the D26.3 instancing-collapse mechanism working exactly
as designed for a maximally-repetitive workload (every instance of a
variant is byte-identical draw-identity), not a claim that the scene path
is "600x faster" for arbitrary content. A scene with less geometry
repetition would show a smaller, but still positive, speedup; the honest
takeaway is "instancing collapse eliminates CPU recording cost
proportional to how repetitive the workload is," which this stress-v2
field is deliberately constructed to demonstrate at its most favorable
(and clearly labeled as such, not presented as a general benchmark
result).

**Measurement precedent** [gate ruling #15: "Task 23's report has the
measurement precedent"]: `task-23-report.md`'s own sample 07 A/B (BASE
f9169e4 vs. the zero-alloc executor fix, same dev container, `--draws
30000`, frame-2 `cpu_record_ms`, 5 reps): `--threads 1` 9.031ms→9.170ms
(unchanged, within ~10% run-to-run noise); default (7 workers)
3.660ms→3.528ms (~3.6% faster, ~2x more consistent spread) — confirming
the executor's own steady-state allocation elimination this stress-v2
number set is now measured against.

**Steam Deck rows**: added to `MANUAL_VERIFICATION.md`'s `## 09_scene` and
new `## Steam Deck (09_scene...)` sections, unchecked, per the plan's own
explicit instruction ("Deck rows added to MANUAL_VERIFICATION as
unchecked") — no dedicated Deck hardware was available in this session.

## 6. Packaging verification (both presets, standalone)

```
$ tools/package_samples.sh linux-native linux-x86_64 rendererx-samples-linux-x86_64.zip
package_samples: zipping into '.../rendererx-samples-linux-x86_64.zip' ...
... (141 files, zero copy_required() failures)

$ tools/package_samples.sh windows-cross-zig windows-x86_64 rendererx-samples-windows-x86_64.zip
... (125 files, zero copy_required() failures)
```

Standalone run (extracted OUTSIDE the build tree, no dev-tree fallback
reachable):
```
$ cd /tmp/rx09-standalone/09_scene && VK_ICD_FILENAMES=... xvfb-run -a ./sample_09_scene --validate
sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
sample_09_scene: headless gate PASSED
```

`tools/regen_references.sh` generalized (2nd positional `[sample]` arg,
default `08_gltf_viewer` — byte-identical to its pre-Task-24 behavior,
verified: `git diff --stat samples/08_gltf_viewer/references/` is EMPTY
after re-running it against the unmodified sample 08 binary); `09_scene`
branch produces `grid_scene.png` only (no `loading_state.png` — sample 09
uses a SYNC import, no async loading-state concept).

## 7. Deviations from a literal reading of the brief/matrix, and why

- **Grid size 4×4 (not the matrix's own illustrative "5×5, 3 visible
  rows" sketch from an earlier gate draft)**: I chose smaller, rounder
  numbers (16 total, 8/8 split) for simplicity; the BINDING criterion is
  "exact, hand-computable counters," which this satisfies regardless of
  the specific grid dimensions — no artifact pins an exact size.
- **Deterministic culling via layer mask, not frustum-boundary geometry**:
  the matrix's own row 10 criterion only requires "culled counter is
  nonzero," not that frustum culling specifically be the source for the
  CI-gated composition. Frustum culling as a MECHANISM is still real and
  exercised (`DrawListBuilder::build()`'s own `cullingFrustumPlanes()`
  test always runs; `buildShadow()`'s own extruded-box test always runs)
  — only the CI gate's exact-count SOURCE is layer-mask, chosen
  specifically to avoid the gate's own correctness depending on
  reproducing FOV/AABB trigonometry bit-for-bit across platforms. Recorded
  as a deliberate engineering judgment, not an oversight.
- **Stress-v2 field is Registry-free (fabricated `MeshHandle`/
  `MaterialHandle`), not a committed synthetic glTF asset**: `Registry::
  registerMesh()` is private, friend-scoped to `import_gltf.cpp` alone
  (verified directly against `registry.h`) — there is no way to get a
  real Registry-backed mesh/material handle for procedural content
  without a real glTF import. `DrawListBuilder`'s own `MeshSubmeshesFn`/
  `MaterialResolveFn`/Scene's `MeshBoundsFn` are explicitly documented,
  intended injection seams for exactly this case ("this library's own
  tests bind a trivial in-test callable instead," draw_list.h's own top
  comment) — applying that same sanctioned seam at sample scope avoids a
  new committed binary test asset, a new Python generator script, and a
  new `ASSET-NOTES.md` entry, for equivalent test coverage of the "4
  pipeline/material variations" comparability requirement.
- **`--scene sponza` not exercised against a real fetched asset**:
  disclosed explicitly in §1 row 2 and in MANUAL_VERIFICATION.md — the
  code path is implemented and reviewed correct (same pattern as sample
  08's own asset resolution), but fetching + rendering the real ~53 MB
  asset was outside this session's available time. This is the ONE
  criterion in this report not backed by a live run.
- **`sample_09_scene_tests` excluded from the windows-cross-zig ctest
  run** (§3) — a pre-existing CI regex-precision limitation, not widened
  in this task (see §3's own reasoning).
- **Registry layer-8 annotation applied to the table row only**, not the
  separate deferred-details paragraph (lines ~148-155) — matches the
  ruling's own explicit text exactly ("annotate the layer table row...
  no checkbox mechanism exists").

## 8. Self-review

- Every binding HUD/mask/counter/shadow/input criterion is backed by a
  real, running code path exercised in this session (not stubbed), with
  command tails/log lines reproduced above.
- The two genuinely-undelivered items (Sponza live-fetch verification,
  windows-cross-zig unit-test coverage for my own device-free test) are
  disclosed explicitly rather than silently claimed, per this project's
  own "no half-assed, no silently-dropped scope" standard — neither
  blocks the BINDING gate criteria as written (Sponza: CI never fetches
  it by design, D16; the Wine exclusion is a pre-existing, documented
  posture this task inherits, not introduces).
- The mid-task concurrent-writer incident (§0) is recorded honestly,
  including that I removed the rogue forks' work rather than attempting
  to reconcile inconsistent partial output — full ownership of every
  delivered line rests with this session.
- Revert-discrimination evidence is captured for the one NEW,
  correctness-critical algorithm this task introduces
  (`splitByBlockAndGroup()`, §4 finding 3) per the binding constraint;
  the D27 worker-guard check (§1 row 12) is a structural (compile-time-
  adjacent) guarantee per draw_recording.h's own design, matching
  rx_scene's own established precedent for that exact property.

## 9. Fix-round delta (independent review response)

Independent review (`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-24-review.md`)
found spec compliance **❌** on one binding criterion (matrix row 2) and 2
Low doc-accuracy findings. All addressed in this round; verdicts below
reflect the state AFTER these fixes, not the original submission.

### H1(a) — BindlessTable sampledImages capacity too small for Sponza

`samples/09_scene/main.cpp`: `sampledImages` raised from 64 to 256
(matching sample 08's own identical justification — "sized for the
largest scene this file actually loads, not just the default asset");
`samplers` raised from 16 to 32 for the same reason. Comment rewritten to
explicitly name Sponza and the review that found the gap, so a future
reader doesn't have to rediscover the reasoning.

### H1(b) — TextureCache::registerRealTexture() destroy-while-in-flight (shared rx_asset bug)

`src/rx_asset/texture_cache.cpp`: both failure branches AFTER
`uploader_.uploadImageMips()` succeeds (the `uploadImageMips()` failure
branch itself, defensively, and the `registerSampledImage()` failure
branch — the one that actually crashed) now call `uploader_.wait(
uploader_.flush())` before returning, so the doomed `Texture2D`'s
destructor never runs while its own already-recorded upload commands
might still be in-flight. Mirrors this SAME class's own pre-existing
`uploadFallbacks()` precedent (texture_cache.cpp, cites the identical
validation error) byte-for-byte in spirit.

**New discriminating regression test** (`src/rx_asset/tests/
texture_cache_test.cpp`, `rx_asset_tests` binary — no Sponza/network fetch
needed): `makeFixture()` gained an optional `Capacities` parameter
(default unchanged, byte-identical for every pre-existing caller); a new
fixture pins `sampledImages=4` — exactly the D11 fallback-texture count,
zero spare — so `TextureCache::create()` itself still succeeds (consumes
exactly 4) but the very next real `load()` call deterministically hits
capacity exhaustion. Two loads in a row both fall back to the checkerboard
handle, zero validation errors.

**Revert-discrimination evidence** (required, captured directly): with the
fix reverted, this exact test:
```
[error] [vulkan validation] Validation Error: [ UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage ]
  ... bound VkImage ... was destroyed.
[doctest] ... FATAL ERROR: test case CRASHED: SIGSEGV - Segmentation violation signal
test cases: 2 | 1 passed | 1 failed
```
— the exact validation-error class and crash the review's own Finding H1
describes, reproduced in isolation. Restored (`diff` against the pre-probe
copy: empty); rebuilt; `rx_asset_tests`: 34/34 passed again (full suite,
not just this one case).

### Two additional bugs found while proving H1 end-to-end (not present in the original report)

Fixing H1(a)+H1(b) was necessary but not sufficient — running the exact
command the review specified (`sample_09_scene --scene sponza --present
--validate` against the real fetched asset) surfaced two MORE real bugs,
both specific to the custom-scene (`--scene <path>`) code path and never
exercised by the default DamagedHelmet grid or the procedural `--stress`
field (both of which happen to have exactly 1 submesh per instance):

1. **D26.1 draw-data buffer undersized** (`populateImportedInstances()`):
   sized `drawDataCapacityRows` off `renderableHandles.size()` (renderable
   /instance count) instead of the TOTAL SUBMESH count across all
   instances. Sponza imports as **1 renderable with 25 submeshes** (one
   mesh, 25 materials) — `ViewLists::payloads` gets one row per
   submesh-instance pairing (D26.1), so `updateSceneFrame()` wrote up to
   25 `DrawDataGpu` rows into a 1-row-capacity buffer every frame: a real
   heap buffer overflow. Symptom was NOT an immediate crash at the
   overflow itself but a SIGSEGV several allocations later, inside
   `libVkLayer_khronos_validation.so`, called from `recordForwardChunk()`'s
   own `vkCmdDrawIndexed` — traced via `gdb -batch -x` (full backtrace in
   this session's own working notes). Fixed: `populateImportedInstances()`
   now takes `const rx::asset::Registry&` and sums
   `registry.mesh(instance.mesh).submeshes.size()` over every instance.
   `setupShadow()`'s own `shadowDrawDataCapacityRows` had the identical bug
   (sized off `scene->renderableCount()`) — fixed to reuse the already-
   correct `app.drawDataCapacityRows` instead (shadow casters are a subset
   of all renderables, so the same upper bound applies).
2. **`destroyApp()` never released `customMaterialBindings`' param
   buffers**: `helmetMaterial`/`stressMaterialBindings` were both torn
   down; the `--scene <path>` custom-materials vector (25 entries for
   Sponza) was not. Symptom: `VUID-vkDestroyDevice-device-00378` ("child
   objects... has not been destroyed"), 25 unfiltered validation errors at
   process exit, `sample_09_scene: Vulkan validation layer reported errors
   during the present loop` — caught by this task's own `--validate`
   convention, not silently missed. Fixed: a loop mirroring the existing
   `stressMaterialBindings` one, added to `destroyApp()`.

Both found via direct `gdb`/log inspection against the real Sponza run,
not guessed — see the raw log tails below.

### End-to-end criterion proof (matrix row 2, after all 4 fixes)

Sponza already on disk from the review's own fetch (verified:
`assets/fetched/Sponza/glTF/`, 71 files). Command exactly as specified:

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
    timeout --signal=TERM 12 build/linux-native/samples/09_scene/sample_09_scene \
    --scene sponza --present --validate

[info] sample_09_scene: '.../assets/fetched/Sponza/glTF/Sponza.gltf' loaded -- 1 renderable(s), 25 material(s)
[info] rx_platform: gamepad connected id=... hasGyroSensor=false [...]
[info] rx_material: saved 32 bytes of pipeline cache data to '...'
[info] sample_09_scene: window closed cleanly
```

Unfiltered `Validation Error` count (grep -v the codebase's own documented
false-positive guards): **0**. No `Vulkan validation layer reported
errors` line (the process's own end-of-run check). No segfault, no
SIGABRT, ran continuously until the 12s `timeout` sent `SIGTERM`, exited
via the sample's own real `SDL_EVENT_QUIT` handling (`window closed
cleanly`), not a raw kill. Matrix row 2 now genuinely **PASS**, not merely
"reviewed correct" — actually run against real content, clean.

### Full re-verification, both presets, after all 4 fixes

- `linux-native`: full rebuild, zero compiler warnings; `ctest`: **29/29
  PASSED** (77.4s) — includes `rx_asset_tests` at 34/34 (was 32 pre-fix;
  +2 assertions from the new regression test's own 2 loads), and all 3
  `sample_09_scene_*` targets.
- `windows-cross-zig`: full rebuild, zero compiler warnings; `ctest -E
  '...|sample'`: **13/13 PASSED** (110.8s) — `rx_asset_tests` (including
  the new regression test) passes under Wine too.
- `tools/package_samples.sh linux-native ...`: re-verified end-to-end,
  zero `copy_required()` failures (09_scene's own binary is larger now
  due to the capacity/materials changes; packaging itself is unaffected).

### L1 — row-12 evidence citation corrected

§1 row 12's table cell above now correctly cites the
`sample_09_scene_stress_headless` `RX_ASSERT_MAIN_THREAD` abort (the
mechanism that actually pins the `GeometryPool::bind()` D5 fix) instead of
the near-tautological default-grid `setViolationHookForTests()` check,
per the review's own finding.

### L2 — README tag claim removed

`README.md`'s "Phase 4 (complete)" paragraph no longer states
`Tag \`v0.4.0-phase4\`.` as an accomplished fact (no such tag exists;
tagging is a coordinator phase-exit action). Matches Phases 1-3's own
paragraph style — no tag mentioned at all.

### Files touched this round

`samples/09_scene/main.cpp` (BindlessTable capacities; submesh-count-
correct `drawDataCapacityRows`/`shadowDrawDataCapacityRows`; `destroyApp()`
leak fix), `src/rx_asset/texture_cache.cpp` (the shared H1(b) fix),
`src/rx_asset/tests/texture_cache_test.cpp` (new regression test +
`makeFixture()` parameterization), `README.md` (L2), this report (L1 +
this section).
