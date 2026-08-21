# Review — Task 5 (#41): Material-path API-gap audit CLOSURE + present-loop centralization

Reviewer round, **FRESH reviewer** (the original T5 review died mid-round to
a machine restart — see `progress.md`'s HALT/RECOVERY entries; this review
carries that reviewer's partial findings forward under explicit attribution,
per the coordinator's dispatch). Commits under review: `8bae5d3..4283286`
(10 commits, base `e607074`, all local/unpushed). Order of authority
followed: `rulings-2026-08-20.md` ("T5 (#41)") > plan (Task 5, Stage 0) >
`gate/matrix-p5t05-material-audit.md` (12 rows + present-loop survey, as
amended by the ruling) > ticket #41 (incl. the 2026-08-20 owner scope-growth
comment). Independent of the implementer for everything marked **[fresh]**
below; everything marked **[inherited]** is carried from the prior
reviewer's completed-before-the-restart work, attributed and not repeated.

Evidence split, per the dispatch's explicit requirement:

- **[inherited]** — verified by the prior reviewer before the restart;
  cited here, not redone.
- **[fresh]** — verified by this reviewer, this round: code reading, greps,
  a full serial lavapipe ctest run, all nine samples' D17 gates re-run
  directly, three real-NVIDIA interactive sessions (Esc-capture toggle,
  09_scene WM-close, 07_stress WM-close), two revert-discrimination proofs
  performed personally (`PerFrameStorageBuffer::write()` re-proof +
  `resolveDrawGroups()`'s zero-alloc capacity check, my own choice), and a
  full commit-hygiene sweep.

---

## Verdict 1 — Spec compliance: **PASS**

Every one of the 12 audit-table rows is delivered per its ruled/matrix
disposition, verified against the actual code (not the report's narrative
alone) for all rows this round.

### Behavior preservation — all nine samples, lavapipe **[fresh]**

Rebuilt `linux-native` (RelWithDebInfo, up to date, `ninja: no work to do`),
then ran every sample's headless D17 gate directly (not merely via ctest's
pass/fail line) to capture the actual failing-pixel counts:

| Sample | Gate | Result |
|---|---|---|
| 01_triangle | readback | PASSED |
| 02_hotreload | headless gate | PASSED |
| 03_bindless_mesh | headless gate | PASSED |
| 04_streaming | headless gate | PASSED |
| 05_multipass | headless gate | PASSED |
| 06_materials | headless gate | PASSED |
| 07_stress | headless gate | PASSED |
| 08_gltf_viewer | D17 `loading_state` / `loaded_scene` | 0/65536, 0/65536 |
| 09_scene | D17 `grid_scene` | 0/65536 |
| 09_scene | C1 shadow discrimination re-proof | 240/65536 differing (expected non-zero) |
| 09_scene | `--stress --stress-draws 64` | headless gate PASSED |

All nine samples: **0 failing pixels**, exit code 0. `git diff --stat` for
`assets/**`, `**/*reference*`, `samples/common/reference_gate.{h,cpp}` across
`e607074..4283286` is empty — the reference machinery and every reference
image are byte-untouched; no gate was regenerated to make this pass. This
closes the "behavior preservation" requirement outright: ❌ would have
required any nonzero delta or a touched reference — neither occurred.

### The 12 audit rows — verified individually **[fresh]**

1. **Material-param arena (promote → `rx_material`).** Real promotion:
   `src/rx_material/include/rx_material/param_arena_factory.h`/`.cpp`
   contains the actual `vkCreateDescriptorSetLayout` + `DescriptorArena`
   construction; both `samples/08_gltf_viewer/main.cpp` and
   `samples/09_scene/main.cpp` call
   `rx::material::createDemandSizedMaterialParamArena()` through a thin
   sample-local wrapper (`createMaterialParamArena()`, App-state/logging
   only). `grep -n vkCreateDescriptorSetLayout` on both samples' `main.cpp`
   returns zero hits — no local descriptor-set-layout construction remains.
   Naming avoids the `ParamArena` collision per Open Questions #1.
2. **Per-FIF draw-data buffer (promote → `rx_rhi_vk`).** Real promotion:
   `rx::rhi::PerFrameStorageBuffer` composes only `rx_rhi_vk` types.
   08_gltf_viewer's `drawDataBuffer` and 09_scene's **both**
   `drawDataBuffer` and its own internal second duplicate
   `shadowDrawDataBuffer` are all converted to the real type (confirmed by
   grep against both `main.cpp` files — genuine field declarations, real
   `::create()`/`::write()`/`::bindlessIndex()`/`::release()` call sites,
   not a typedef alias). **[inherited]** the file itself was code-reviewed
   clean and the per-FIF ordering invariant (fence-wait before write)
   independently confirmed by the prior reviewer.
3. **`mouse_capture.h` (promote → `rx_platform`).** `samples/09_scene/mouse_capture.h`
   is deleted (confirmed: file does not exist).
   `samples/09_scene/main.cpp` uses `rx::platform::MouseCaptureToggle`/
   `mouseDeltaDrivesCamera`/`escTogglesCapture` via `using` declarations —
   real consumption, not a shim.
4. **`fly_camera.h` (promote → `rx_scene`).** `samples/09_scene/fly_camera.h`
   deleted (confirmed). `main.cpp` uses `rx::scene::FlyCamera`/
   `keyboardDrivesCamera`/`flyCameraLocalMoveDelta` for real — the sample's
   own `App::flyCamera` field is the promoted type.
5. **`grid_layout.h` (rule-sample-local-stands).** Confirmed: the file
   still exists at `samples/09_scene/grid_layout.h`, untouched, and **no**
   equivalent (`gridTransform`/`gridInstanceTransform`) exists anywhere
   under `src/` — a direct repo-wide grep for both symbol names under
   `src/` returns zero hits. The ruling ("stays sample-local") was honored
   exactly, not silently promoted anyway.
6. **`splitByBlockAndGroup()`/`resolveDrawGroups()` (promote + zero-alloc, →
   `rx_scene`).** `samples/09_scene/draw_recording.h`/`.cpp` deleted
   (confirmed). Both functions now live in
   `src/rx_scene/include/rx_scene/draw_list.h` as zero-alloc out-param APIs
   (`RecordSpan`, `materialIndexForSpan()` alongside them). The sample's own
   `App::recordSpanScratch` is a
   `std::vector<std::vector<rx::scene::RecordSpan>>` indexed by chunk index
   — genuinely per-worker-slot scratch, not one shared buffer (matches the
   matrix's explicit worker-safety acceptance criterion). Capacity **and**
   `.data()` pointer-identity tests exist for both functions
   (`draw_list_test.cpp:1330,1357`), plus a dedicated concurrent
   per-worker-slot-safety test (`:1383`) — Task 23's methodology followed
   exactly. **[inherited]** the prior reviewer verified per-worker-slot
   thread-safety against the chunk-0-synchronous executor guarantee.
   **[fresh]** I independently re-proved the zero-alloc discrimination
   myself — see Revert-discrimination below.
7. **`window_resize.h` (absorbed into present-loop centralization).**
   `samples/09_scene/window_resize.h` deleted (confirmed). Its three pure
   decision functions (`pixelSizeRequiresRecreate`, `f11TogglesFullscreen`,
   `shouldSkipTeardownAfterDeviceLoss`) live verbatim in
   `rx_frame_loop/present_loop.{h,cpp}`, each still device-free and
   unit-tested in `pure_decisions_test.cpp` (9 cases).
8. **Present-loop shared helper — module home.** Delivered exactly per the
   ruling: `src/rx_frame_loop/`, layered `rx_platform + rx_rhi_vk + rx_graph
   + rx_core` PUBLIC, identical shape to `rx_debug_ui`. `PresentLoop::create()`/
   `runFrame()`/`recreateAndDependents()` is the single orchestration funnel;
   a D5 main-thread-only statement is present in the header's own top
   comment.
9. **Consistent `SurfaceLost` handling.** `present_loop.cpp` has explicit,
   top-level `if (acquire.status == SwapchainStatus::SurfaceLost)` and
   `if (presentStatus == SwapchainStatus::SurfaceLost)` branches (not a
   nested method check) in both `runFrame()` and `recreateAndDependents()`,
   in the same priority order as `device.cpp`'s own check order
   (SurfaceLost → Suspended → NeedsRecreate → DeviceLost). Cross-checked
   directly against the pre-migration `samples/09_scene/main.cpp` at
   `e607074` (both the acquire-path and present-path SurfaceLost branches):
   behavior is preserved exactly, including the subtlety that the
   present-path branch does **not** call `window->abandonNativeHandle()`
   itself (relies on `recreateAndDependents()` having already done so) —
   this matches the pre-migration comment "matches the sample-local
   original exactly" literally, byte-for-byte reasoning. **See the row-9
   adjudication below for the one part of this row's acceptance criterion
   that is NOT fully delivered.**
10. **Extent-recompile-skip contract.** Delivered exactly per the ruling,
    inside `RenderGraph::compile()` itself (`render_graph.cpp:298-322`):
    caches `lastCompileInfo`/`lastCompiledGeneration`, early-returns `false`
    on a byte-identical, generation-unchanged call.
    `declarationGeneration` is bumped by both `addPass()` and
    `setBackbufferSource()` (the two topology-mutating entry points),
    verified by direct read. Five dedicated tests in `test_compile.cpp`
    (skip on unchanged; recompile on genuine extent change; recompile on
    format/layout-only change; recompile after topology change with
    byte-identical `CompileInfo`; recompile after `reset()`), including a
    pointer-identity check (not content-equality) on the compiled resource,
    matching this project's own D26 discrimination-test discipline.
11. **Deletion of the nine duplicated hand-written loops.** Re-ran the
    report's own proof myself: `grep -rln "SwapchainStatus::NeedsRecreate"
    samples/*/main.cpp` → zero files, exit code 1. Confirmed independently.
12. **CLI-signature unification.** Confirmed by direct grep: all of
    01-06's `main.cpp` now declare `int runPresent(const Args& args)`
    (previously three positional bools/enum); 07-09 were already
    `Args&`-shaped.

**Present-loop module code review [fresh, not covered by the prior
reviewer's "six files"].** Read `src/rx_frame_loop/include/rx_frame_loop/present_loop.h`
(361 lines) and `present_loop.cpp` (272 lines) in full — this is the
largest new surface in the round and was not among the six files the prior
reviewer had already cleared. Findings: clean. `create()` rejects malformed
`CreateInfo` (null device/surface/window, or exactly one of graph/executor
set); `recreateAndDependents()` follows the exact same
SurfaceLost→Suspended→(rebuild)→onRecreate→compile/realize sequence the
pre-migration sample-local lambda used, including the "onRecreate before
graph recompile" ordering the header's own comment documents; `runFrame()`'s
fence-wait-before-frameBody ordering (the I1 precondition every
`PerFrameStorageBuffer::write()` caller depends on) is structurally
guaranteed — `vkWaitForFences` happens unconditionally before the
`FrameContext` is ever constructed or `frameBody` invoked. No leaks on the
`create()` failure paths (VkImageView vector is torn down via
`destroySwapchainViews()` on early return spots that matter). No
new/changed line touches a core `rx_*` library's own required surface.

### CMake link closure and boundary-check mechanism

**[inherited]** No core `rx_*` library links `rx_frame_loop` — this round I
re-confirmed it directly: `grep -rn "rx_frame_loop" src/*/CMakeLists.txt`
returns nothing outside `src/rx_frame_loop/CMakeLists.txt` itself; every
`target_link_libraries(... rx_frame_loop ...)` hit is in a `samples/*/CMakeLists.txt`.
Clean.

**[inherited, restated as the bound finding — see Verdict 2]** The
Task-21 configure-time boundary-check mechanism
(`cmake/DependencyBoundaryCheck.cmake`, `rx_assert_target_excludes_dependency`)
was never extended to assert this new boundary automatically. **[fresh]** I
read the mechanism and its one call site (`CMakeLists.txt:140-142`) myself:
the `foreach` loop only ever checks `rx_core rx_rhi_vk rx_graph rx_material
rx_platform rx_task rx_shader rx_asset rx_scene` against the single
forbidden substring `imgui` — there is no equivalent
`rx_assert_target_excludes_dependency(<core-target> rx_frame_loop)` call
anywhere. The boundary holds today only by hand-verified convention, not by
a configure-time guarantee — exactly the gap the mechanism itself was built
to eliminate for the ImGui boundary. Confirmed real, not a stale claim.

---

## Row-9 adjudication (task item 6) — **explicit ruling**

Matrix row 9's proposed acceptance criterion has two clauses:

> Helper's status-handling switch has an explicit `SurfaceLost` case (not
> merely a nested method check) as its own binding acceptance criterion;
> **a targeted regression test constructs a `Device` already in the
> surface-lost state and asserts the helper's very first
> `acquireNextImage()` call is handled without falling through to frame
> recording.**

**Ruling: the first clause is fully satisfied; the second clause, read on
its own terms, is NOT satisfied by what shipped, and the report's own
self-disclosed concern is correct, not overcautious.**

- Clause 1 (explicit top-level branch, not nested) — **delivered**, verified
  directly above (row 9). This alone would justify a plain PASS on the
  row's core intent (the actual defect class the row exists to close —
  01-08's "works by structural coincidence" pattern — is gone, replaced by
  a single, explicit, correctly-ordered branch consumed by all nine
  samples).
- Clause 2 (a targeted regression test for "`Device` already lost before
  the very first `acquireNextImage()` call") — **not delivered**. I read
  both `src/rx_frame_loop/tests/present_loop_gpu_test.cpp` and
  `pure_decisions_test.cpp` in full: no test case constructs a `PresentLoop`/
  `Device` pair already in the surface-lost state and then calls
  `runFrame()` for the first time against it. The existing GPU suite's
  Suspended-retry test (`present_loop_gpu_test.cpp:220`) exercises a
  *different* entry condition (loss discovered mid-session, after N
  successful frames). `Device` itself (`device.h`) offers no test-only
  seam to force `surfaceLost_ = true` before a first real call either — the
  only way to reach that state today is a genuine `recreateSwapchain()`
  call against an already-dead surface, which is precisely what would need
  to be synthesized for this specific test and was not.

The report's own "Concerns for the coordinator" item 1 already discloses
this honestly, in-round, rather than silently passing it off — I verified
the disclosure is accurate rather than merely trusting it.

**Weighing this against the standing no-deferred-fixes rule:** this gap
does not qualify for legitimate deferral under
`feedback_no_deferred_fixes.md` — it is neither a feature phase-fit nor a
human-hardware-only `MANUAL_VERIFICATION` row; it is squarely a
device-free/GPU-testable methodology gap that the matrix named explicitly.
At the same time, it is narrow and low-risk in substance: the code path
`clause 2` would exercise is the *same three-line branch*
(`acquire.status == SurfaceLost → abandonNativeHandle() → surfaceLost_ =
true → return SurfaceLost`) already exercised, for real, by the nine
real-NVIDIA WM-close trials (2 of which I personally reproduced this round
— see below) and by the pre-loop `if (surfaceLost_) return
Result::SurfaceLost;` defense-in-depth guard at the top of `runFrame()`,
which IS a form of "first call against an already-lost loop" coverage,
just not synthesized from a freshly-`create()`d `PresentLoop` whose
underlying `Device` was pre-seeded lost before `create()` itself ever ran.

**Verdict on this row: PASS on spec compliance** (the disposition —
"promote, fold into the shared helper" — is delivered, and the acceptance
criterion's primary, load-bearing clause is met), **with a standing Medium
finding carried into Verdict 2**: the named regression-test methodology was
not written and should be closed with a small follow-up (either a new
`Device`-level test-only seam to force `surfaceLost_` pre-`create()`, or a
GPU test that closes the real window before ever calling `PresentLoop::create()`
and asserts the first `runFrame()` short-circuits) rather than carried
indefinitely as a "future hardening round" item, per the standing
no-deferred-fixes posture. This does not flip the row, or the task's overall
spec-compliance verdict, to ❌ — the functional substance is proven, on real
hardware, nine times; only the specific unit/GPU-test artifact the matrix
named is missing.

---

## Concern 3 adjudication (task item 7) — 07_stress's `#74` fast-exit path

**Verified directly on real NVIDIA hardware this round — expected for its
workload, not a masked hang, with one honest caveat.**

Launched `sample_07_stress --present --validate` (default `--draws 30000`,
confirmed via `main.cpp:130`) on the real NVIDIA GeForce RTX 2080
(`nvidia-smi -L` confirmed the driver, DISPLAY=:1). Steady state ran at
`fps≈3.6, cpu_record_ms≈270, draws=30000`. Closed the window via `xdotool
windowclose`. Log sequence:

```
Device::recreateSwapchain: surface-lost inferred from VkResult=-3 -- ... [Issue #73]
Device::recreateSwapchain: surface capabilities query failed ... entering the surface-lost terminal state ... [Issue #73]
rx_frame_loop: the present window's native handle is gone -- stopping without touching the surface further [Issue #73]
VkDevice reports lost immediately after the present window's native handle was already known gone -- skipping further Vulkan teardown and letting process exit reclaim GPU resources directly [Issue #74]
```

Process exited cleanly (confirmed gone via `ps`/`pgrep`, zero core files).
Zero unfiltered validation errors (every "error"-substring hit in the log is
the already-classified `known false positive` marker or the Issue #73
informational line's own literal mention of a VkResult name). This exactly
matches the report's predicted mechanism: with the GPU still saturated by
the 30000-draw workload at the moment the window disappears,
`vkDeviceWaitIdle()` observes `VK_ERROR_DEVICE_LOST` stacked on the
already-known surface loss, and `shouldSkipTeardownAfterDeviceLoss()`
correctly routes to the `std::_Exit()` fast path rather than attempting
(and hanging or erroring inside) the ordinary per-object teardown. **Not a
masked hang** — the mechanism exists specifically because the *alternative*
(ordinary teardown proceeding regardless) is what would risk real,
unfiltered "still in use" validation errors against a device that
`vkDeviceWaitIdle()` no longer actually waits for.

**Honest caveat, worth the coordinator's awareness, not a T5 regression:**
between the last steady-state `stress: fps=...` log line and the
surface-loss detection, there was a gap of roughly 17 seconds with no
further per-second stats logged — noticeably longer than 09_scene's
sub-second detection latency in the same trial methodology. This is
consistent with 07_stress's heavy default workload reducing how often the
loop reaches an `acquireNextImage()`/`present()` call that could observe
the driver's lost-surface status, combined with `Device::recreateSwapchain()`'s
reactive-only detection design (no proactive/event-driven signal — the
present-loop survey itself already documents that no SDL event fires for a
third-party window destroy). This detection path is entirely
`device.cpp`-owned and untouched by this diff (T5 only relocated the
*orchestration* around it into `rx_frame_loop`); it is not a T5-introduced
characteristic, and the process did not hang indefinitely — it self-resolved
with correct telemetry. Flagged here as an observation for the coordinator's
awareness, not a spec-compliance or code-quality finding against this task.

---

## Real-NVIDIA interactive verification (task item 4) **[fresh]**

Performed solo, foreground, NICEd, serialized (desktop confirmed idle
before and clean after each trial; zero stray processes; zero core dumps).

**Sample 09 Esc-capture toggle.** Launched `sample_09_scene --present
--validate` on real NVIDIA. Screenshotted three states via `xdotool
key Escape` + `import -window`:
1. Initial: HUD reads `Mouse: CAPTURED` (matches `MouseCaptureToggle`'s
   documented captured-by-default contract).
2. After one Esc: HUD reads `Mouse: RELEASED (Esc: toggle release/recapture,
   click viewport: recapture)`.
3. After a second Esc: HUD reads `Mouse: CAPTURED` again.

Toggle behavior is correct end-to-end through the migrated present loop
(`escTogglesCapture()`/`toggleOnEscPressed()` wiring, driven from
`PresentLoop`-orchestrated event pumping). Zero unfiltered validation
errors in the session log.

**Sample 09 WM-close trial.** Closed the same session's window via `xdotool
windowclose`. Log shows the identical Issue #73 sequence
(`surface-lost inferred` → `entering the surface-lost terminal state` →
`rx_frame_loop: ... stopping without touching the surface further`) followed
by `sample_09_scene: window closed cleanly` — the ordinary (non-`#74`)
exit path, as expected for a light workload where the GPU is idle by the
time the window disappears. Process fully exited, zero unfiltered
validation errors, no core file.

**07_stress WM-close trial.** Covered above under Concern 3.

**Not independently re-verified this round:** the other seven samples'
(01-06, 08) individual real-NVIDIA WM-close trials. The dispatch scoped
this round's real-hardware interactive work to "sample 09 Esc-capture
toggle + ONE WM-close trial" plus the separate 07_stress verification for
concern 3 — deliberately narrow, consistent with the standing
desktop-verification-serialization rule's spirit of minimizing GPU/display
disruption to what each item actually requires. The other seven samples'
trials are accepted on the implementer's own report evidence (all nine
funnel through the identical, single `rx_frame_loop` code path this review
DID exercise directly on two representative samples — the "proving ground"
09_scene and the heaviest-workload, both-#74-branches-exercised 07_stress —
plus all nine samples' D17 gates, which this round re-ran directly).

---

## Revert-discrimination (task item 5) **[fresh, performed personally]**

**`PerFrameStorageBuffer::write()` write-after-fence re-proof.** Edited
`src/rx_rhi_vk/src/per_frame_storage_buffer.cpp`'s `write()` to hardcode
`Buffer& buffer = *buffers_[0];` (ignoring `frameSlot`), rebuilt
`rx_rhi_vk_tests`, ran the cross-slot-isolation test directly:

```
per_frame_storage_buffer_test.cpp:133: CHECK( std::memcmp(slot0Data, blobA.data(), blobA.size()) == 0 ) is NOT correct!  values: CHECK( 1 == 0 )
per_frame_storage_buffer_test.cpp:137: CHECK( std::memcmp(slot1Data, blobB.data(), blobB.size()) == 0 ) is NOT correct!  values: CHECK( -1 == 0 )
per_frame_storage_buffer_test.cpp:144: CHECK( std::memcmp(rx::rhi::detail::debugSlotBufferData(*perFrame, 1), blobB.data(), blobB.size()) == 0 ) is NOT correct!  values: CHECK( -1 == 0 )
1 test case, 3 assertions failed (13 passed)
```

Exactly the predicted failure (slot 0 corrupted by slot 1's write, byte-level
readback catches it immediately). Reverted (`git diff --stat` on the file:
empty), rebuilt the full project, `rx_rhi_vk_tests` and the full 31-test
suite green again.

**Second revert, my own choice: `resolveDrawGroups()`'s zero-alloc capacity
invariant.** Chosen because it is the newest test methodology in this round
(the Task-23 capacity-snapshot pattern applied to the sample-recording
path for the first time) and was not part of the prior reviewer's six
already-cleared files. First attempt (`outGroups.clear();
outGroups.shrink_to_fit();`) did **not** reliably fail the test — a false
negative caused by glibc's allocator handing the exact same address/size
back on the immediately-following reallocation for this test's small
(3-group) working set, exactly the allocator-reuse risk the test's own
header comment names. Switched to a decisive break
(`outGroups.reserve(outGroups.capacity() + 1);` immediately after
`clear()`, guaranteeing monotonic growth by construction rather than
depending on allocator behavior):

```
draw_list_test.cpp:1352: CHECK( outGroups.capacity() == capacityAfterFirst ) is NOT correct!  values: CHECK( 23 == 8 )
draw_list_test.cpp:1352: CHECK( outGroups.capacity() == capacityAfterFirst ) is NOT correct!  values: CHECK( 24 == 8 )
... (monotonically increasing every iteration)
1 test case, 40/41 assertions failed
```

Confirms the capacity check discriminates correctly against a genuine
"not actually zero-alloc" regression. Reverted (`git diff --stat`: empty),
rebuilt, `rx_scene_tests`: 81/81 cases, 6587/6587 assertions pass again.
**Worth noting for the record, not a finding against this task:** my first,
subtler probe (`shrink_to_fit()`) slipping past the test for this small
dataset is a real, if narrow, illustration that the capacity-only half of
this discrimination pattern is weaker than its own documentation implies
for small N — the `.data()` pointer-identity half is the one actually doing
the discrimination work in that scenario, and it also happened not to catch
`shrink_to_fit()`+reallocation-to-the-same-address on this run. This is a
pre-existing characteristic of the Task-23 pattern itself (used unchanged
from `executor.h`'s own precedent), not something T5 introduced or should
be required to fix — noted for awareness, not a T5 finding.

**Full project rebuild + full ctest re-run after both probe/revert cycles:
31/31 tests passed** (see below) — no collateral damage from either probe.

---

## Full serial ctest, lavapipe (task item 8) **[fresh]**

Two full runs this round (before and after the revert-discrimination
probes), both 31/31:

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:1 nice -n 10 ctest -j1 --output-on-failure
...
100% tests passed, 0 tests failed out of 31
Total Test time (real) = 181.55 sec        [pre-probe run]
...
100% tests passed, 0 tests failed out of 31
Total Test time (real) = 152.97 sec        [post-probe/revert run]
```

---

## Commit hygiene (task item 8) **[fresh]**

- **Author/committer identity**: all 10 commits `Yousef Wadi
  <ywadi85@gmail.com>` — matches `git config user.name`/`user.email`
  exactly, both author and committer fields, every commit.
- **No AI attribution**: `git log e607074..4283286 -p | grep -iE
  "claude|anthropic|co-authored|generated.by|ai.assist|openai|gpt|chatgpt"`
  returns exactly one hit — the task-05-report.md's own prose describing
  that it checked for this ("...claude/anthropic/co-authored/generated-by/
  ai-assistant) — none found.") — not an actual attribution. Clean.
- **Nothing pushed**: `git log origin/main..main --oneline | wc -l` = 10;
  `git branch -vv` confirms `main` is `[origin/main: ahead 10]`. All ten
  commits are local-only.
- **Pathspec coherence**: `git show --name-only` on all nine implementation
  commits shows each scoped exactly to its named module (plus root
  `CMakeLists.txt`/`.github/workflows/ci.yml` only where genuinely needed
  for wiring in the new module/exclusion pattern) — no commit reaches into
  an unrelated module's files. The tenth commit (`4283286`) touches only
  `task-05-report.md`.
- **No SDD/plan/board files swept in**: none of the nine implementation
  commits touch `.superpowers/sdd/**` or `docs/superpowers/plans/**` —
  confirmed by `git log --name-only` filtered against those paths; only the
  closing report commit does, and only its own report file. Matches the
  report's self-disclosed near-miss (a `git reset --soft` + selective
  `git restore --staged` recovery on the first `rx_frame_loop` commit
  attempt) having been caught and fixed before it left the working tree —
  I could not re-observe the near-miss itself (it never landed), but the
  clean final state is consistent with the report's account.
- **Working tree left as found**: `git status --short` before and after
  this entire review is identical — only the pre-existing
  `progress.md` modification (left alone per instructions) and the three
  stray `rx_compute_pipeline_test*.cache` root files (queued for cleanup
  separately, left alone per instructions). Every temporary edit I made
  during the two revert-discrimination probes was restored byte-identically
  (`git diff --stat` on both touched files: empty, both times).

---

## Verdict 2 — Code quality: **APPROVED, with findings**

No blocking defects. Two findings carried, plus one informational
observation — none require re-opening the task before closure, but should
be tracked.

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | Task-21 configure-time dependency-boundary-check mechanism (`cmake/DependencyBoundaryCheck.cmake`) was not extended to assert automatically that no core `rx_*` library transitively links the new `rx_frame_loop` module — the boundary holds today by manual verification only (confirmed clean, both by the prior reviewer and independently by me this round), not by a configure-time guarantee. | Medium | **[inherited]**, re-confirmed **[fresh]** |
| 2 | Matrix row 9's second acceptance-criterion clause — a targeted regression test constructing a `Device` already surface-lost before `PresentLoop`'s very first `acquireNextImage()` call — was not written. Functional coverage of the same code path exists (nine real WM-close trials, two independently reproduced this round; the pre-loop `surfaceLost_` guard). Self-disclosed honestly in the report rather than hidden. Does not block this task's closure on its own, but per the standing no-deferred-fixes posture should be closed with a small follow-up rather than left open-ended. | Medium | **[fresh]** adjudicated above |
| 3 (informational) | 07_stress's real-NVIDIA WM-close trial showed a ~17-second gap between window destruction and the loop detecting surface loss (vs. sub-second for the lighter 09_scene trial) — self-resolves correctly via the `#74` fast-exit path, not a hang, and the detection mechanism itself (`Device::recreateSwapchain()`'s reactive-only classification) is untouched by this diff. Noted for awareness only. | Info | **[fresh]** |

Everything else reviewed — the six previously-cleared files
**[inherited]**, the new `rx_frame_loop` module in full **[fresh]**, all 12
audit-table dispositions **[fresh]**, the CMake link closure **[inherited +
fresh]**, both revert-discrimination proofs **[fresh]**, all nine samples'
D17 gates **[fresh]**, and commit hygiene across all ten commits
**[fresh]** — is clean.

---

## Summary for the coordinator

- **Spec compliance: PASS.** All 12 audit rows delivered per their ruled
  disposition; behavior preserved byte-identically on every one of nine
  samples' D17 gates; `grid_layout.h` correctly left sample-local and not
  promoted anywhere; the present-loop centralization itself (rows 7-12) is
  real, consumed by all nine samples, and the nine duplicated hand-rolled
  loops are provably gone (grep, zero hits).
- **Code quality: Approved, with two Medium findings and one informational
  observation** (table above) — none blocking, both Medium findings already
  self-disclosed in-round by the implementer and independently confirmed
  real by this review rather than dismissed.
- **Row 9 adjudicated**: primary clause (explicit `SurfaceLost` branch)
  delivered; the named regression-test methodology clause was not, and that
  gap is real — ruled Medium, not a spec-compliance failure, given the
  strength of the real-hardware functional coverage already performed.
- **07_stress's `#74` exit**: confirmed expected for its workload on real
  NVIDIA hardware, not a masked hang, with an honest ~17s detection-latency
  observation noted for awareness (pre-existing `device.cpp` characteristic,
  outside this diff's scope).
- **Not independently re-verified this round**: samples 01-06/08's
  individual real-NVIDIA WM-close trials (accepted on the implementer's
  report, given the shared code path was directly exercised on two other
  samples plus all nine D17 gates); the exact moment-of-catch for the
  implementer's own self-disclosed commit-sweep near-miss (never landed,
  so nothing to observe directly — the clean final state is consistent with
  the account).

---

## Addendum — scoped re-check of the two Medium closures, `4283286..47c9b16`

Commits under this addendum: `d23a047` (build/cmake), `e8e1875`
(test/rx_frame_loop), `47c9b16` (report). Scope, per the coordinator's
dispatch: verify ONLY that the two Medium findings from the review above
are closed — not a re-review of the whole task. Package:
`review-4283286..47c9b16.diff`.

### Medium #1 — boundary-check mirrored for `rx_frame_loop`

**ADDRESSED.** Read the diff first: `CMakeLists.txt` gains a second
`foreach(_rx_core_target ...) rx_assert_target_excludes_dependency(${_rx_core_target}
rx_frame_loop) endforeach()` loop, same nine-target list as the existing
`imgui` check, immediately after it; `cmake/DependencyBoundaryCheck.cmake`'s
`FATAL_ERROR` message is generalized (no longer hardcodes
"core-libraries-stay-ImGui-free" — now names the actual call site/forbidden
value).

Re-proved the injection myself, independent of the report's own transcript:

1. Injected `target_link_libraries(rx_material PUBLIC rx_frame_loop)`
   immediately after `add_subdirectory(src/rx_material)` (`CMakeLists.txt:103`).
   Reconfigured (`cmake .` in `build/linux-native`):
   ```
   CMake Error at cmake/DependencyBoundaryCheck.cmake:138 (message):
     [dependency-boundary-check] 'rx_material' transitively depends on something
     matching 'rx_frame_loop' -- this violates a core-library dependency
     boundary (see the rx_assert_target_excludes_dependency(rx_material
     rx_frame_loop) call site in the root CMakeLists.txt for which boundary and
     its rationale).  Dependency chain: rx_material -> rx_frame_loop
   -- Configuring incomplete, errors occurred!
   ```
   Matches the report's own captured output exactly, including the
   generalized (no longer ImGui-hardcoded) message text.
2. Removed the injection. `git diff --stat -- CMakeLists.txt`: empty.
   Reconfigured clean: `-- Configuring done` / `-- Generating done`, zero
   errors.
3. Also independently re-created the documented recursion-guard quirk:
   injected `target_link_libraries(rx_platform PUBLIC rx_frame_loop)`
   instead (`rx_frame_loop` depends on `rx_platform`, so this creates a
   genuine `rx_platform -> rx_frame_loop -> rx_platform` cycle). Reconfigure
   hit the walk's own recursion-depth guard exactly as documented:
   ```
   CMake Error at cmake/DependencyBoundaryCheck.cmake:47 (message):
     [dependency-boundary-check] recursion depth exceeded walking
     'Threads::Threads' -- likely a cyclic or self-referential LINK_LIBRARIES
     entry; investigate before raising this limit.
   ```
   — not the clean chain message, confirming the in-code usability note
   (`CMakeLists.txt`'s new comment: pick a target `rx_frame_loop` does NOT
   itself depend on) is accurate, not an unverified claim. Reverted;
   `git diff --stat -- CMakeLists.txt`: empty; reconfigured clean again.

Both injections' restorations verified byte-identical, both reconfigures
clean. Mechanism is load-bearing, proven twice, independent of the
implementer's own transcript.

### Medium #2 — row 9's regression-test artifact

**ADDRESSED.** Read `device.h`/`device.cpp`: `rx::rhi::detail::forceSurfaceLostForTesting(Device&)`
is a real test-only friend seam (`friend void
detail::forceSurfaceLostForTesting(Device& device);` on `Device`, one-line
body `device.surfaceLost_ = true;`), matching the established
`debugSlotBufferData()`/`debugFrameBufferData()` carve-out convention
already used elsewhere in this task. The new `present_loop_gpu_test.cpp`
TEST_CASE constructs a `PresentLoop` against a healthy `Device`, then calls
this seam to force the `Device` (not the loop) surface-lost BEFORE the
loop's own first `runFrame()` — exactly the ordering matrix row 9's
acceptance criterion names, and correctly distinct from
`PresentLoop::surfaceLost_`'s own top-of-function guard (which does not
catch this case, since the loop has never itself observed a loss — the
branch actually under test is the acquire-status check deeper in
`runFrame()`).

Ran `rx_frame_loop_gpu_tests` directly on lavapipe: **8/8 test cases,
117/117 assertions, zero unfiltered validation errors** — matches the
claimed counts exactly.

**Did not re-run the deadlock revert** (removing `runFrame()`'s
`SurfaceLost` branch), per the coordinator's explicit instruction — the
report's own transcript already documents a real, reproduced GPU deadlock
requiring `SIGKILL` from that revert, and repeating a SIGKILL-class hazard
adds no verification value. Instead performed the requested **safe
mutation**: commented out the test's own forced-lost setup (the
`forceSurfaceLostForTesting()` call and its immediately following
`REQUIRE(fixture->device.isSurfaceLost())`), leaving the `Device` genuinely
healthy, rebuilt, and ran the single test case directly:

```
present_loop_gpu_test.cpp:314: CHECK( result == Result::SurfaceLost ) is NOT correct!  values: CHECK( 0 == 3 )
present_loop_gpu_test.cpp:315: CHECK_FALSE( frameBodyCalled ) is NOT correct!  values: CHECK_FALSE( true )
present_loop_gpu_test.cpp:322: CHECK( fixture->device.acquireCallCount() == acquireBefore ) is NOT correct!  values: CHECK( 1 == 0 )
present_loop_gpu_test.cpp:328: CHECK( loop->isSurfaceLost() ) is NOT correct!  values: CHECK( false )
present_loop_gpu_test.cpp:332: CHECK_FALSE( fixture->context.hasValidationErrors() ) is NOT correct!  values: CHECK_FALSE( true )
1 test case, 5/10 assertions failed
```

Every real assertion in the test fails against the healthy device exactly
as predicted (`runFrame()` actually ran a real frame — `Result::Ok`
(`0`), `frameBody` was invoked, a real `acquireNextImage()` happened, the
loop never latches `surfaceLost_`) — no hang, no SIGKILL needed, process
returned control normally. This is independent, first-hand confirmation
that the test's assertions are load-bearing, not vacuously true. Reverted
(`git diff --stat` on the test file: empty), rebuilt, re-ran: 8/8 cases,
117/117 assertions, clean again.

### Full verification after both re-proofs

- Full project rebuild: `ninja: no work to do` after each revert (clean,
  no residual state).
- Full serial lavapipe ctest, run after all probes were reverted:
  **100% tests passed, 0 tests failed out of 31** (133.73s).
- `git status --short` before and after this addendum's work is identical
  to the main review's baseline — only the pre-existing `progress.md`
  modification and the three stray `rx_compute_pipeline_test*.cache` files
  remain, both left untouched per standing instructions. Every temporary
  edit (two `CMakeLists.txt` injections, one `present_loop_gpu_test.cpp`
  mutation) was restored byte-identically, confirmed via `git diff --stat`
  after each.

### Commit hygiene, `4283286..47c9b16`

- **Pathspec coherence**: `d23a047` touches only `CMakeLists.txt` +
  `cmake/DependencyBoundaryCheck.cmake`; `e8e1875` touches only
  `src/rx_frame_loop/tests/present_loop_gpu_test.cpp` +
  `src/rx_rhi_vk/include/rx_rhi_vk/device.h` +
  `src/rx_rhi_vk/src/device.cpp`; `47c9b16` touches only
  `task-05-report.md`. No cross-contamination.
- **Author/committer**: all three `Yousef Wadi <ywadi85@gmail.com>`,
  matching local git config, both fields.
- **No AI attribution**: `git log 4283286..47c9b16 -p | grep -iE
  "claude|anthropic|co-authored|generated.by|ai.assist|openai|gpt|chatgpt"`
  — zero hits.
- **Nothing pushed**: `git log origin/main..main --oneline` = 13 (the
  original 10 plus these 3); `git branch -vv`: `[origin/main: ahead 13]`.

### Addendum verdict: **ALL ADDRESSED**

Both Medium findings from the original review are closed, verified
independently (not merely re-trusted from the report):

- **Medium #1 (boundary check)** — ADDRESSED. Mechanism mirrored correctly,
  message generalized, load-bearing (FATAL_ERROR with the correct
  dependency chain on injection, clean on removal) proven twice by me this
  round, including the documented cyclic-injection edge case.
- **Medium #2 (row 9 regression test)** — ADDRESSED. Real test-only seam,
  correct ordering construction, counts match (8/8, 117/117) on lavapipe,
  and the test's discriminating power independently confirmed via a safe
  (non-deadlocking) mutation rather than repeating the already-proven
  SIGKILL-class hazard.

No new findings surfaced during this scoped re-check. The task's overall
verdicts from the main review above stand unchanged: **spec compliance
PASS; code quality APPROVED** (the informational 07_stress latency
observation is explicitly out of scope for this addendum and remains a
watch item, not a finding against this closure).
