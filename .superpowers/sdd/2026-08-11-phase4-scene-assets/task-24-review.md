# Task 24 review — samples/09_scene: scene fly-through + stress-v2 + release prep (card #15)

Independent review. Did not write this code. Reviewed commits (local, not
pushed): 3911397 (sample), 702bda5 (packaging/regen tooling), df5c955 (CI
stress-v2 numbers step), 2c92bea (MANUAL_VERIFICATION/README/registry
docs), b0aaff5 (report). Authority order followed:
`gate/rulings-2026-08-18.md` §#15 + RC rulings + Errata > spec >
`gate/matrix-issue15-sample09.md` > `task-24-brief.md`'s task text (its
"Execution notes (coordinator)" tail ignored per instructions).

Build/test machine: this repository's own checkout at
`/media/ywadi/second/renderer_x` (also bind-mounted at
`/home/ywadi/d2/renderer_x` — same filesystem, confirmed via build-log
paths). `linux-native` preset, Ninja/RelWithDebInfo, `RX_DEBUG_CHECKS=ON`.
GPU tests run via `xvfb-run -a` with `VK_ICD_FILENAMES` pinned to this
box's real lavapipe ICD (`/usr/share/vulkan/icd.d/lvp_icd.json`) — the
box also exposes a real GPU driver by default, which the sample's own D17
gate correctly treats as "informational only, not enforced" when it isn't
lavapipe; forcing lavapipe explicitly reproduces CI's own posture and is
what all pass/fail numbers below are measured against.

## Empirical work performed

- Full `cmake --build --preset linux-native`: succeeds, only 09_scene's 4
  new targets compiled (rest already built).
- `ctest --preset linux-native` (serial, lavapipe-forced): **29/29
  PASSED**, ~73s. Matches the report's own count exactly, including the 3
  new targets (`sample_09_scene_headless`, `sample_09_scene_stress_headless`,
  `sample_09_scene_tests`).
- Ran `sample_09_scene --validate` and `--validate --stress --stress-draws
  64` directly (not just via ctest): both print `headless gate PASSED`;
  `D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true` under
  lavapipe; exact counters match the report's §2 table verbatim
  (imported=16, culled=8, visible=8, recordsIn=8, drawsSubmitted=1;
  stress: recordsIn=64, drawsSubmitted=4, chunkCount=7).
- Counted all `Validation Error`/`Validation Warning` lines vs. lines NOT
  matching the codebase's own `(known false positive: ...)` prefix, for
  both the default grid run and the stress run: **zero unfiltered
  validation errors** in both. Spot-checked the guard functions in
  `src/rx_rhi_vk/src/context.cpp` — narrow, multi-substring-matched, and
  any unmatched validation message still increments the real error counter
  and logs as `[error]` (not silently dropped) — confirms the "zero
  unfiltered" posture is real, not a broad catch-all.
- Reproduced the A/B stress-v2 numbers myself (`sample_07_stress --draws
  30000 --threads 1|default` vs. `sample_09_scene --stress --stress-draws
  30000 --threads 1|default`): same order of magnitude as the report
  (07: ~10-12ms / ~3.5-7ms; 09: ~0.02ms / ~0.03-0.05ms; recordsIn=30000,
  drawsSubmitted=4 both configs) — run-to-run noise only, no discrepancy.
- Ran `tools/package_samples.sh linux-native linux-x86_64 <zip>` myself:
  141 files, zero `copy_required()` failures, matches report §6.
  Extracted the zip OUTSIDE the build tree and ran `sample_09_scene
  --validate` standalone under lavapipe: `headless gate PASSED`, D17 gate
  0 failing pixels.
- Windows-cross-zig: build artifacts already present
  (`sample_09_scene.exe`/`sample_09_scene_tests.exe`, 09_scene staged with
  material/shadow shaders); ran `sample_09_scene_tests.exe` under `wine`
  myself — **5/5 passed**, confirming it is NOT a real platform
  incompatibility, only excluded from Wine ctest by the pre-existing regex
  (see adjudication #3).
- **Revert-discrimination #1 (required)**: replaced `splitByBlockAndGroup()`
  with a naive groups-only split (ignoring block boundaries, using the
  first block found for the whole group). Rebuilt and ran
  `sample_09_scene_tests`: **exactly 2 of 5 cases fail** — `REQUIRE(
  spans.size() == 2 )` → actual 1, and `REQUIRE( spans.size() == 3 )` →
  actual 2 — the other 3 pass. Matches the report's own claimed numbers
  exactly. Restored the file; `diff` against the pre-sabotage copy is
  empty; rebuilt; all 5 tests pass again.
- **Revert-discrimination #2 (required)**: reverted `HudState`'s
  constructor to the pre-fix `layerRowVisible.fill(true)` bug (report
  finding #1). Rebuilt and ran the headless gate directly: fails hard —
  `culled=0 != expected 8`, `visible=16 != expected 8`, `recordsIn=16 !=
  expected 8`, `collapse ratio 0.937500 != expected 0.875000` on every one
  of 3 frames, AND the D17 pixel gate independently fails
  (`failingPixels=563/65536`) — process exits 1. Restored the file; `diff`
  empty; rebuilt; gate passes again.
- **Bonus revert probe (GeometryPool::bind() D5 fix, report finding #2)**:
  reverted `recordForwardChunk()` to call the guarded
  `app.geometryPool->bind(cmd, span.blockId)` directly instead of the raw
  `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer` calls. Under the
  *default grid* scene this sabotage is **silently unobserved** (exit 0,
  gate PASSED) — because the grid's single collapsed draw means chunk 0
  (which the executor documents as always running on the main thread,
  never concurrently with other chunks) handles 100% of the work, so no
  worker chunk ever reaches the guarded call. Under
  `--stress --stress-draws 64` (drawsSubmitted=4, spread across multiple
  non-empty worker chunks), the SAME sabotage **immediately SIGABRTs**:
  `rx_core: main-thread-only API called from a non-main thread:
  GeometryPool::bind` (exit 134) — i.e. `sample_09_scene_stress_headless`
  (a real, already-registered ctest target) *does* structurally pin this
  fix, via `RX_ASSERT_MAIN_THREAD`'s abort, even though the report's own
  row-12 evidence text describes a different, narrower mechanism (see
  Finding L1 below). Restored the file; `diff` empty; rebuilt; passes.
- **Revert probe (report finding #4, ImGui first-appearance window
  sizing)**: removed the `ImGui::SetNextWindowSize(...,
  ImGuiCond_FirstUseEver)` call. Rebuilt and ran the default headless
  gate: reproduces the exact bug it fixes — `HUD overlay pass produced no
  draw data (CmdListsCount=0, TotalVtxCount=0)`, gate FAILED, exit 1.
  Restored; `diff` empty; rebuilt; passes.
- Read `main.cpp` (2937 lines), `draw_recording.h/.cpp`,
  `tests/test_draw_recording.cpp` in full; read the CI/packaging/
  MANUAL_VERIFICATION/README/registry diffs in full; spot-checked
  `GeometryPool::bind()`'s D5 guard, `debug_checks.h`'s violation-hook
  contract, the render graph's chunk-0-runs-on-main-thread documentation,
  and `TextureCache::registerRealTexture()`'s bindless-registration
  failure path (see Finding H1).
- Fetched Sponza myself (`tools/fetch_assets.sh --sponza`, network access
  confirmed available) and ran `sample_09_scene --scene sponza --present
  --validate` under Xvfb — see **Finding H1**, the one substantive result
  of this review.
- Commit hygiene: all 5 commits authored/committed as `Yousef Wadi
  <ywadi85@gmail.com>`; grepped every commit message body for AI-
  attribution strings (claude/anthropic/co-authored/assistant/generated
  by) — zero matches; pathspecs are cleanly scoped per commit (sample
  code / tooling / CI / docs / report, no cross-contamination); `git log
  origin/main..main` confirms these 5 commits are ahead of origin,
  nothing pushed.
- Restored every temporary edit made during sabotage probes to
  byte-identical originals (verified via `diff` after each restore,
  reproduced above); rebuilt after each restore. Final `git status
  --short` shows only the pre-existing `.superpowers/sdd/.../progress.md`
  modification that predates this review — untouched, left alone per
  instructions.

## Finding H1 (HIGH) — `--scene sponza` crashes; matrix row 2's binding
criterion is not actually met

Matrix row 2's acceptance criterion (adopted verbatim by this task, and
listed as **PASS** in the report's own §1 row 2, albeit with the caveat
"NOT independently verified against a real fetched Sponza"): *"`--scene
sponza` fails loudly... when present, loads and renders without
validation errors."* Ruling text (`gate/rulings-2026-08-18.md` doesn't
override this row; the matrix criterion stands as adopted).

I fetched the real Sponza asset (`tools/fetch_assets.sh --sponza`,
succeeded, 71 files) and ran
`sample_09_scene --scene sponza --present --validate` under Xvfb+lavapipe
myself. **It segfaults** (exit 139/`Segmentation fault (core dumped)`),
preceded by a stream of real, UNFILTERED validation errors (not matching
any of the codebase's known-false-positive guards):

```
rx::rhi::BindlessTable::registerSampledImage: sampled-image capacity (64) already fully occupied; rejecting
rx_asset: TextureCache: BindlessTable::registerSampledImage failed (capacity exhausted?)
rx_asset: TextureCache: 'material#20 normal' failed to load ... falling back to the D11 checkerboard
[vulkan validation] Validation Error: [ UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage ] ...
  You are adding vkCmdPipelineBarrier2 to VkCommandBuffer ... invalid because bound VkImage ... was destroyed.
```

Root cause, traced directly:

1. `samples/09_scene/main.cpp:815` sizes the sample's `BindlessTable` with
   `sampledImages=64` — the accompanying comment explicitly justifies this
   as "generous headroom for the helmet's 5 PBR textures..." and never
   mentions Sponza, even though the very same file implements and ships
   the `--scene sponza` path. The sibling sample that already handles
   real, variable-size glTF content (`samples/08_gltf_viewer/main.cpp:824`)
   sizes the identical field at `sampledImages=256`. Real Sponza (~25
   materials × up to 3 textures each) exhausts 64 partway through import
   (first failure at material#20 of the log).
2. When `BindlessTable::registerSampledImage()` fails,
   `TextureCache::registerRealTexture()` (`src/rx_asset/texture_cache.cpp:
   188-239`) returns an invalid handle at line 210 — but the local
   `Texture2D` it already uploaded via `uploader_.uploadImageMips()`
   (line 200, BEFORE the bindless-registration attempt) is never moved
   into a live `Entry`; it is destroyed at scope exit. The already-
   recorded (not-yet-submitted/completed) upload commands then reference
   a destroyed `VkImage`, producing the "invalid because bound VkImage
   was destroyed" validation errors and the eventual segfault. This is a
   genuine, previously-unexercised lifecycle bug in shared
   `rx_asset::TextureCache` failure-handling code (not sample-09-local
   code) — no earlier sample/test has ever driven bindless capacity to
   exhaustion during a real import, so this path was never hit before.

This is not a cosmetic gap. The report's own characterization — "code-
reviewed correct... not independently verified" (§1 row 2) and "the code
path is implemented and reviewed correct... a disclosed scope gap, not a
claimed pass" (§7) — is factually incorrect on the "reviewed correct"
half: it is not correct, it crashes. `resolveSponzaScenePath()`'s own
loud-failure-when-absent behavior (matrix row 2's other half) is fine and
was not the part in question.

Two independent fixes are needed: (a) size `samples/09_scene`'s
`BindlessTable::Capacities` generously enough for the scenes this file
actually supports (matching or exceeding sample 08's 256, or computing it
from the actual imported material/texture count), and (b) fix
`TextureCache::registerRealTexture()`'s bindless-registration-failure
path so a rejected texture doesn't leave already-recorded upload commands
referencing a destroyed image — this second half is shared-library code
and is the more consequential of the two (any future consumer that
exhausts bindless capacity mid-import hits the same crash regardless of
what capacity number sample 09 picks).

This is the sole finding that changes the spec-compliance verdict.
Everything else in the per-criterion table (§1 rows 1, 3-32) was verified
directly and holds.

## Findings — other

- **L1 (LOW, doc/test-design accuracy).** Report §1 row 12's evidence
  text ("D27 main-thread pre-resolution -- worker-guard test... wrapped
  around one real chunked-recording frame under 7 workers; zero
  violations captured") describes the `setViolationHookForTests()` check
  at `main.cpp:2528-2549`. I verified this check is real infrastructure
  but, as currently exercised (default grid: a single collapsed draw
  entirely handled by chunk 0, which the render graph documents as always
  running on the main thread), it can never actually trip regardless of
  whether `resolveDrawGroups()`'s own main-thread invariant holds —
  `resolvePipeline()` is structurally only ever called from
  `updateSceneFrame()` on the main thread, so this specific check is a
  near-tautology for the scenario it runs against, not a demonstrated
  revert-discrimination (unlike finding #3's `splitByBlockAndGroup()`
  tests, which the report correctly claims discriminate and which I
  independently reproduced). This does NOT mean D5/D27 is unprotected in
  practice — the *actual* threading danger the report's finding #2
  describes (`GeometryPool::bind()` from a worker chunk) IS caught, hard,
  by `RX_ASSERT_MAIN_THREAD`'s abort under `sample_09_scene_stress_headless`
  (see the bonus revert probe above) — just not by the mechanism row 12's
  own evidence text points to. Recommend the report/comment be corrected
  to cite the right protecting mechanism, or add an explicit multi-chunk
  fixture that would actually trip the hook. Not a functional gap.
- **L2 (LOW, doc accuracy).** `README.md`'s new "Phase 4 (complete)"
  paragraph ends with "Tag `v0.4.0-phase4`." — no such tag exists yet
  (`git tag -l` confirmed, only through `v0.3.0-phase3`), and tagging is
  explicitly a coordinator action outside this task's scope per the
  report's own §1 row 32. Phases 1-3's own paragraphs (checked via `git
  show 47135af:README.md`, the commit that added Phase 3's identical-
  shape paragraph before that phase's own tag existed) never name a tag
  at all — this is a new, unprecedented phrasing that reads as an
  already-accomplished fact. Cosmetic; trivially fixed by dropping the
  sentence or rephrasing as forward-looking.

No other findings — Medium/other Low items were not found; the rest of
the implementation is exemplary (see per-criterion confirmation below).

## Per-criterion confirmation (beyond the two required reverts)

Verified directly, matching the report's own table row-for-row: exact
headless counters (§2); zero unfiltered validation errors under lavapipe,
grid and stress; 29/29 ctest; A/B stress-v2 numbers same order of
magnitude, jointly reported (wall-clock AND recordsIn/drawsSubmitted on
the same log line — CI's own `stress-v2-numbers.txt` step preserves this
joint format; sample 07's own report format untouched); the report's own
§5 "reading these numbers honestly" paragraph correctly frames the ~600×
gap as a drawsSubmitted (4 vs 30000) artifact of a deliberately
maximally-repetitive workload, not a general speedup claim — no
misrepresentation found anywhere (report, CI comments, or artifact
format); venue labeling ("this dev container", not desktop/Deck-grade)
matches Task 23's own precedent; Deck rows in `MANUAL_VERIFICATION.md`
are present and explicitly unchecked; HUD ships two visibly distinct mask
controls (`cullMaskFromToggles()` → `camera.cullMask` u32 vs. a single
light-channel checkbox → `Scene::setLightChannels` u8, never conflated,
confirmed by direct code read); vsync toggle calls the identical
`setPresentMode`+`recreateSwapchain` pair the CLI flag uses; 60-frame
rolling FPS (`kRollingWindow=60`); packaging includes 09 in both the zip
list and staging loop plus its extra material/shadow/references/
DamagedHelmet staging block; header comment says "nine" (was "eight");
packaged 09 verified standalone by me directly; 07_stress
MANUAL_VERIFICATION section added; stale `09_fly_through` references
corrected to `09_scene` everywhere I found; README file-tree/Roadmap
additions are factual (module/sample lists match what's actually in the
tree); registry row-8 annotation is EXACTLY
`"(delivered: Phase 4 — scene submission/culling; LOD remains deferred)"`
as mandated by the ruling, diff-verified.

## Adjudication of the 4 disclosed concerns

1. **§0 fork-race incident — coherent authorship?** VERIFIED clean. Read
   `main.cpp`, `draw_recording.h/.cpp`, and the test file in full: no
   duplicate/orphaned function definitions, no leftover
   `KHR_materials_unlit`/"StandardPBR-only" inconsistency the incident
   describes, no TODO/FIXME/XXX markers, one consistent comment voice and
   cross-referencing convention throughout. Nothing in the committed diff
   reads as foreign or half-merged.
2. **`--scene sponza` not exercised against a real fetched asset —
   acceptable disclosure, or a gap requiring fetch+run now?** The ruling
   text governs: matrix row 2's own criterion is *"when present, loads
   and renders without validation errors"* — a functional requirement on
   the code path, not merely on whether the implementer's own session
   happened to run it. Fetching and running it myself (see Finding H1)
   shows the path **does not meet its own binding criterion** — it
   crashes. This is NOT an acceptable disclosure of an untested-but-
   presumably-working gap; it is a genuine, reproducible functional
   defect that the report's "code-reviewed correct" framing incorrectly
   downgrades to a mere verification gap. Requires a fix, not just a
   MANUAL_VERIFICATION checkbox.
3. **`sample_09_scene_tests` swept into windows CI's blanket `sample`
   exclusion regex — pre-existing limitation, or must it be fixed here?**
   PRE-EXISTING, confirmed via `git blame`: the exact exclusion line
   (`ctest --preset windows-cross-zig -E
   'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample'`) was
   introduced by commit `bbd1df1` (Task 21, `feat(rx_debug_ui): ...`),
   before Task 24 existed, and this task's own `ci.yml` diff never
   touches that line. The report's reasoning (don't widen a shared,
   already-green CI mechanism without a specific mandate) is sound. I
   independently ran `sample_09_scene_tests.exe` under `wine` myself —
   5/5 pass — confirming this is purely a CI regex-precision gap, not a
   real Wine/platform incompatibility. Acceptable as-is; a future,
   separately-scoped CI hygiene pass could tighten the regex (e.g. to
   `_headless$|_gpu$|_stress_headless$`) to restore Windows coverage for
   device-free sample tests, but that is not this task's gate criterion.
4. **The 5 in-development bug fixes — each in-scope and tested?** All 5
   are in-scope (found and fixed during this task's own new code,
   touching only files this task owns). Tested, confirmed by direct
   revert:
   - #1 (HudState default layer mask) — tested; revert-discrimination #2
     above reproduces it exactly.
   - #2 (GeometryPool::bind() D5 violation) — real and load-bearing;
     tested, though not by the mechanism the report's row 12 cites (see
     Finding L1) — by the bonus revert probe above, which SIGABRTs under
     `sample_09_scene_stress_headless` when reverted.
   - #3 (`resolveDrawGroups()` blockId blind spot /
     `splitByBlockAndGroup()`) — exemplary; tested, revert-discrimination
     #1 above reproduces the report's exact claimed numbers (2 of 5
     tests fail).
   - #4 (ImGui first-appearance window sizing) — tested; my own revert
     probe reproduces the exact `CmdListsCount=0` symptom the report
     describes, gate correctly fails.
   - #5 (missing `beginFrame()` pairing before the D27 guard's
     `captureFrame()`) — in-scope, and its effect is folded into finding
     #4's own test (the report's own account of root-causing #4 back to
     #5 is consistent with what I observed); no separate dedicated test
     exists for #5 alone, but it is not separately load-bearing outside
     of #4's own already-tested symptom.

## Not independently verifiable in this environment

- Real Steam Deck hardware runs (both `09_scene` and the platform/HUD
  MANUAL_VERIFICATION rows) — correctly left unchecked; no Deck hardware
  available here either.
- The exact wall-clock stress-v2 numbers as PUBLISHED artifacts in a real
  GitHub Actions CI run (only reproduced locally, same methodology,
  consistent order of magnitude — did not execute the actual CI
  workflow).
- Real human-observed `--present` sessions (mouse drag feel, HUD
  legibility, gamepad hot-plug) — matches the file's own disclosed
  "functionally verified under Xvfb, not human-observed" posture; I did
  not attempt a real windowed session either.

## Verdicts

**Spec compliance: ❌** — one binding criterion (matrix row 2,
`--scene sponza` "loads and renders without validation errors") is
empirically false: the path segfaults against a real fetched Sponza
asset (Finding H1). All other 31 criteria rows were independently
verified and hold. This is a narrow, well-isolated, non-CI-gated
(present-mode-only) failure — fixing Finding H1 (bindless capacity sizing
in `samples/09_scene/main.cpp` + the `TextureCache::registerRealTexture()`
failure-path lifecycle bug in shared `rx_asset` code) is sufficient to
flip this verdict.

**Code quality: Approved with findings** — 1 High (H1, blocks spec
compliance until fixed — see above), 2 Low (L1 doc/test-design accuracy,
L2 doc accuracy), 0 Medium. Everything else — the D26.3/D27 recording
path, the `splitByBlockAndGroup()` fix and its tests, the HUD's two
distinct mask controls, packaging, CI wiring, docs, commit hygiene — is
production-quality, well-tested, and honestly reported.

---

# Fix-round re-review (commits c4dce74, 1ea8a01, 1fe2e6f)

Scoped re-review of the fix round responding to this file's own findings
above. Package: `review-b0aaff5..1fe2e6f.diff` (4 files: `task-24-report.md`
delta, `README.md`, `samples/09_scene/main.cpp`,
`src/rx_asset/tests/texture_cache_test.cpp`,
`src/rx_asset/texture_cache.cpp`).

## Empirical work performed this round

- Full rebuild, `linux-native`: clean, zero warnings.
- `rx_asset_tests` (lavapipe-forced): **34/34 passed**, 576 assertions —
  matches the delta's own claimed count (32→34, +1 test case × 2
  assertions from the new capacity-exhaustion regression test) exactly.
- **Revert-discrimination on H1(b) (required)**: removed the
  `uploader_.wait(uploader_.flush())` call from the
  `registerSampledImage()` failure branch specifically — the branch the
  delta itself identifies as "the one that actually crashed" — leaving
  the `uploadImageMips()`-failure branch's own call untouched (surgical,
  matching the claim under test). Rebuilt and ran the new TEST_CASE alone:
  reproduces the exact claimed failure — a real, unfiltered
  `UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage`
  ("bound VkImage ... was destroyed") validation error, immediately
  followed by `FATAL ERROR: test case CRASHED: SIGSEGV`, process exit 139.
  Restored via `cp` from a pre-sabotage copy; `diff` (both against my own
  saved copy and via `git diff`) confirms byte-identical; rebuilt;
  `rx_asset_tests` 34/34 again.
- `ctest --preset linux-native` (serial, lavapipe-forced): **29/29
  PASSED**, ~76s — no regression from the fix round.
- **Re-ran the exact Sponza criterion myself** (Sponza already on disk
  from the original round's fetch): `VK_ICD_FILENAMES=.../lvp_icd.json
  xvfb-run -a timeout --signal=TERM 15 sample_09_scene --scene sponza
  --present --validate`. Result: `'.../Sponza.gltf' loaded -- 1
  renderable(s), 25 material(s)`, ran for the full 15s under real frame
  traffic, `window closed cleanly` on the delivered SIGTERM→
  `SDL_EVENT_QUIT` path (process exit 124 is `timeout`'s own documented
  convention for "I had to deliver the signal," not a hang or crash
  indicator — the log's own clean-exit line is the real signal). **Zero**
  validation-error/warning lines outside the codebase's own documented
  false-positive guards, across an 887-line log spanning ~13s of
  continuous rendering. No segfault, no SIGABRT, no
  `VUID-vkDestroyDevice-*` lines (confirms the `customMaterialBindings`
  leak — extra bug 2 — is actually gone, not just claimed gone). Matrix
  row 2's binding criterion is now genuinely met.
- Windows-cross-zig: rebuilt the touched targets (`rx_asset`,
  `sample_09_scene`) — clean, no errors. (Did not re-run the full Wine
  ctest suite this round; trusting the delta's own 13/13 claim, consistent
  with this round's narrower empirical scope as specified.)
- Scope check: `git show --stat` on all 3 commits — `c4dce74` touches only
  `texture_cache.cpp`+its test; `1ea8a01` touches only
  `samples/09_scene/main.cpp`; `1fe2e6f` touches only the report delta +
  README. No unrelated files, no scope creep.
- Commit hygiene: all 3 authored/committed as `Yousef Wadi
  <ywadi85@gmail.com>`; grepped every message body for AI-attribution
  strings — zero matches; `git log origin/main..main` shows 8 commits
  ahead (the original 5 + these 3), nothing pushed.

## Claims verified

1. **[H1(a)] BindlessTable 64→256/16→32.** VERIFIED — `main.cpp:815`
   diff-confirmed; comment correctly names Sponza and cites sample 08's
   own identical 256 as precedent, not an arbitrary number.
2. **[H1(b)] `uploader_.wait(uploader_.flush())` before destroy-on-failure,
   with a discriminating regression test.** VERIFIED — code matches the
   claim exactly (both failure branches, though only the second one was
   ever actually hit in practice); the new test is genuinely
   network-free (a synthetic `sampledImages=4` fixture, not Sponza) and
   deterministic; I independently reproduced the revert-discrimination
   myself (SIGSEGV + the exact validation error), not merely re-read the
   report's own claimed transcript.
3. **[Extra bug 1] `populateImportedInstances()` submesh-count sizing.**
   VERIFIED as fixed and root-caused correctly (`registry.mesh(instance.
   mesh).submeshes.size()` summed across instances, both for
   `drawDataCapacityRows` and — a detail I confirmed was also caught in
   the same round — `setupShadow()`'s own `shadowDrawDataCapacityRows`,
   which had the identical `renderableCount()`-based bug). **Coverage
   ruling** (per the coordinator's own question): I traced the code
   directly — `populateHelmetGrid()` (the default-grid path
   `sample_09_scene_headless` exercises) sizes `drawDataCapacityRows` off
   a **hardcoded `kGridInstanceCount` constant** (main.cpp:1511), a
   completely separate code path from `populateImportedInstances()`; it
   was never bugged in the first place and provides **zero** regression
   coverage for this fix (it happens to be numerically correct only
   because DamagedHelmet has exactly 1 submesh, not because it shares any
   logic with the fixed function). `populateImportedInstances()` is
   reached ONLY via `--scene <path>`, which matrix row 2 itself states
   explicitly is never CI-exercised (D16's own "CI never downloads
   Sponza" rule, adopted verbatim as this ticket's binding criterion's own
   scope limiter — *"CI never exercises this path... only the headless
   DamagedHelmet-grid path is CI-gated"*). Ruling: **the disclosed,
   reproduced manual run satisfies the binding criterion as literally
   written** — the matrix's own wording explicitly exempts this code path
   from CI-gated coverage, and I have now independently re-verified that
   run myself end-to-end (see above), not merely trusted the report's
   transcript. This is NOT a blocking gap for this round's verdict.
   Non-blocking recommendation for a future pass: the underlying property
   (renderable count ≠ submesh count for any multi-submesh mesh) is
   asset-agnostic, not Sponza-specific, and currently has no automated
   regression protection at all (same gap applies to extra bug 2 below,
   which is reached by the exact same code path) — a small, locally
   committed multi-submesh glTF fixture (2-3 submeshes, no network fetch)
   could pin both fixes cheaply, following this same task's own
   `splitByBlockAndGroup()`/H1(b) precedent of "prove the failure mode
   with a synthetic fixture, not just the happy path." Suggested, not
   required.
4. **[Extra bug 2] `destroyApp()` `customMaterialBindings` leak.**
   VERIFIED fixed — code diff-confirmed (mirrors the existing
   `stressMaterialBindings` loop exactly); empirically confirmed via my
   own Sponza re-run above (zero `VUID-vkDestroyDevice-*` lines across the
   full session, where the original run would have produced 25 — one per
   leaked material). Same coverage caveat as extra bug 1 applies (only
   exercised by the `--scene <path>` branch); same non-blocking
   recommendation.
5. **[L1] Report row-12 citation corrected.** VERIFIED — diff-confirmed,
   the corrected text accurately describes the mechanism I identified in
   the original round (the `RX_ASSERT_MAIN_THREAD` abort under
   `sample_09_scene_stress_headless`), not the near-tautological
   default-grid check.
6. **[L2] README tag claim removed.** VERIFIED — diff-confirmed, the
   sentence is gone; Phase 4's paragraph now matches Phases 1-3's own
   style (no tag named).

## Fix-round verdict

**ALL ADDRESSED.**

**Spec-verdict flip:** The original round's spec-compliance verdict
(**❌**, blocked solely by matrix row 2 / Finding H1 — a real,
reproducible segfault against real Sponza content) is hereby flipped to
**✅**. All three components of Finding H1 (undersized bindless capacity,
the shared `TextureCache` destroy-while-in-flight lifecycle bug, and the
two additional defects the fix round's own end-to-end proof surfaced —
submesh-undersized draw-data buffers and a teardown leak) are fixed,
diff-verified, and independently re-run by me against the real fetched
Sponza asset with a clean result (zero unfiltered validation errors, no
crash, clean exit). Code quality remains **Approved with findings**, now
at 0 High / 2 Low (L1, L2 — both corrected, downgraded from "finding" to
"resolved") / 0 Medium, plus the two non-blocking coverage recommendations
above (extra bugs 1 and 2 have no automated regression test, only a
re-verified manual run — acceptable per the matrix's own explicit
CI-exemption wording for this code path, not required to close this
round).
