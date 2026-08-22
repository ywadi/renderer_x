# Task 12 review — Stage 1 exit: viewer upgrade + checkpoint numbers (issue #48)

Independent reviewer round. Commit under review: `365a187`
(`feat(samples/08_gltf_viewer): Stage 1 exit -- HUD env/exposure readout,
packaging fixes (#48)`), base `92eae34`, branch `task/t12-stage1-exit`,
worktree `/media/ywadi/second/renderer_x-worktrees/t12-stage1-exit`.
Reviewer did not write this code. Every claim below was independently
re-derived: full ctest re-run both drivers + Wine, live present-mode
screenshots (offscreen Xvfb, not Xephyr — no on-desktop visible windows),
a real `package_samples.sh` run + fresh-unzip standalone execution of all
nine samples (Linux) plus a live Wine run of the unzipped Windows binary,
a discrimination probe (deleted `brdf.slang` from the unzipped copy), and
a temporary, byte-identically-restored instrumentation patch to split
`--bench-frames`' published number into record-only vs. full-readback
components.

## Verdict 1 — Spec compliance: **NOT clean — 2 closable gaps**

Most of the ticket's acceptance bar is met cleanly: the HUD is built
entirely on `rx_debug_ui::Overlay` (Task 5 audit row verified
zero-tolerance clean, see Finding 4), packaging closes three real
inherited defects with a genuine end-to-end fix (verified below), the
suite is green on both presets/drivers/Wine, the packaged zip is
standalone-verified, and Steam Deck rows are honestly tracked-not-run per
RC8. Two things keep this from a clean PASS, both closable in a fix round
per this repo's no-deferred-fixes policy:

1. **`samples/README.md`'s missing `09_scene` section** (coordinator
   pre-decision, binding per the dispatch — see Finding 3). The brief's own
   "Files" row names README rows; the implementer's own report flags this
   gap but scopes it out as "not this ticket's problem." The coordinator's
   ruling overrides that self-assessment: it counts against this ticket's
   own Files-row completeness.
2. **The published frame-time numbers do not meet the row's own implicit
   bar of being a trustworthy, driver-labeled Stage-1 baseline** (matrix
   row: "Frame times: helmet/sponza/workshop... driver-labeled"). The
   `--bench-frames` methodology conflates render cost with GPU-readback
   machinery cost, and the report's own claim that this "matches" this
   codebase's established measurement convention is factually incorrect
   (see Finding 1). The lavapipe figures additionally fail to reproduce
   within tolerance in a quiet environment (Finding 2). Both are real,
   closable defects in the checkpoint numbers, not the underlying
   engineering work.

## Verdict 2 — Code quality: **NOT Approved — 2 MAJOR, 1 MINOR**

The HUD implementation, the exposure-label bug fix, and the packaging
fixes are all genuinely well-executed and independently reproduced by this
review with real, hands-on evidence (live screenshots, a real
delete-and-rerun discrimination probe, a fresh end-to-end packaged-zip
run including a live Wine execution the report itself scoped out of). The
two MAJOR findings are both in the Stage-1 checkpoint numbers' own
methodology/reproducibility, not in the shipped product code path — but
because CLAUDE.md makes published benchmark numbers an exit criterion that
future CI regression gates (T36) will compare against, a mislabeled or
unreproducible baseline is a real defect, not a nitpick, and should close
before this checkpoint is treated as final.

---

## Finding 1 (MAJOR) — `--bench-frames` conflates render cost with a full
GPU readback round-trip, contradicting the report's own claim of parity
with `07_stress`/`09_scene`'s established convention

**Where:** `samples/08_gltf_viewer/main.cpp`, the `captureFrame` lambda
(diff ~L2739-2765) and the `--bench-frames` loop (diff ~L2952-2976).

**What's wrong:** The report claims (task-12-report.md, "What shipped" #2
and the Numbers section) that `--bench-frames` measures "matching this
codebase's own established measurement convention (`07_stress`'s
`--draws`, `09_scene`'s `--stress` both measure the identical way)". This
is false, and verifiably so by reading those two samples' own code:

- `samples/07_stress/main.cpp` (~L1397-1410): times **only**
  `executor->execute()` inside `runOnce()`'s record callback, with an
  explicit comment: *"NOT `runOnce()` as a whole, which also
  `vkQueueSubmit()`s and `vkQueueWaitIdle()`s... whose GPU wait time has
  nothing to do with CPU recording cost"*. Published as `cpu_record_ms`.
- `samples/09_scene/main.cpp` (~L3143-3149): identical pattern, with an
  explicit comment that this is *"the SAME `cpu_record_ms` metric
  samples/07_stress's own runHeadless() publishes (timed around ONLY that
  call, **not the readback that follows**)"* — done specifically for
  gate ruling #15's cross-sample A/B comparability contract.

`08_gltf_viewer`'s own `captureFrame()` does the opposite: it times the
**entire** call, which is (a) `runOnce()` #1 — record + submit + wait for
the render graph, (b) a **second** `runOnce()` — submit + wait for a
`vkCmdCopyImageToBuffer`, (c) a **freshly allocated** host-visible buffer
every iteration (`app->allocator->createHostVisibleBuffer(...)`, not
reused/pre-allocated), and (d) `readback->invalidate()` + a full
`std::memcpy` + `canonicalizeToRgba8()`. None of this is disclosed beyond
"CPU-record + GPU-submit-and-wait wall time... not vsync-paced
present-mode timing" — which is honest about the vsync gap but omits the
much larger readback-machinery gap.

**Empirical proof (temporary instrumentation, reverted byte-identically
before commit — restored file confirmed via `md5sum` match and `git
status`/`git diff` clean):** added a `recordMsOut` parameter to
`captureFrame()`, mirroring `09_scene`'s own identical pattern exactly,
and logged both the existing `frame_bench` line and a new
`frame_bench_recordonly` line. Real NVIDIA RTX 2080 (580.82.07) results,
200 iterations:

| Scene | full (`captureFrame()`, published) | record-only (`cpu_record_ms`-equivalent) | readback overhead |
|---|---|---|---|
| DamagedHelmet | 1.694 ms avg | 0.208 ms avg | **88%** of the published number |
| Sponza | 6.948 ms avg | 4.232 ms avg | **39%** of the published number |

For the smallest scene (the sample's own default), the published "frame
time" is dominated almost entirely by fixed per-iteration
readback/second-submit cost, not render cost — the opposite of what
`07_stress`/`09_scene`'s own precedent (and gate ruling #15's A/B
contract) was built to guarantee. This is exactly the "benchmark flag
that measures the wrong window" the dispatch's own attention lens warned
about: these numbers become the Stage 1 baseline that T36's future
CI perf-regression gate will diff against.

**Fix direction:** either add a `recordMsOut`-style split (mirroring
`09_scene`'s own precedent, giving a directly-comparable `cpu_record_ms`
alongside the existing full number), or reuse a pre-allocated readback
buffer and clearly re-label the published metric as "render + full GPU
readback round-trip," not "frame time" unqualified. The former is
preferable — it is the established convention this ticket claimed
(incorrectly) to already be following, and costs one lambda-parameter
change (verified working in the instrumented build above).

## Finding 2 (MAJOR) — Published lavapipe frame-time numbers do not
reproduce within tolerance in a quiet environment; the report's own
huge max-spikes go uncaveated

**Where:** task-12-report.md, "Numbers" section, frame-times table.

**What's wrong:** Re-running the exact same measurement (unmodified
binary, same `--bench-frames` iteration counts as the report: 60 on
lavapipe, 200 on NVIDIA; same default `environments/gate_test_env.hdr`
fixture; same asset paths already warm in this worktree) on a quiet host
(load average ~1.1/8 cores, no other GPU-adjacent process running,
confirmed via `nvidia-smi --query-compute-apps` / `ps aux`) gives:

| Scene | Driver | Report avg | Reviewer avg (2-3 clean runs) | Delta |
|---|---|---|---|---|
| DamagedHelmet | lavapipe | 11.799 ms | 4.667 / 4.693 / 4.717 ms | **report ~150% higher** |
| Sponza | lavapipe | 52.134 ms | 30.575 ms | **report ~70% higher** |
| Workshop | lavapipe | 116.589 ms | 107.424 ms | ~8% (within tolerance) |
| DamagedHelmet | NVIDIA RTX 2080 | 1.703 ms | 1.691 / 1.694 ms | <1% (matches) |
| Sponza | NVIDIA RTX 2080 | 7.479 ms | 7.121 / 6.948 ms | ~5% (matches) |
| Workshop | NVIDIA RTX 2080 | 12.332 ms | 15.336 / 15.499 / 15.247 ms | ~24-26% (flagged, mild) |

The two large lavapipe deltas are far outside the ">20% flag" tolerance
given for this review, and the direction is telling: NVIDIA (GPU-bound,
largely insensitive to host CPU contention) reproduces cleanly, while
lavapipe (a CPU-bound software rasterizer, maximally sensitive to host
scheduling noise) does not. My own repeated runs are tight and consistent
(helmet: 4.667/4.693/4.717ms, spread <1.1%; no max-spike outliers at all
— max is only ~1.1-1.2x the min in every run). The report's own numbers
show the opposite signature: huge min/max spreads that look like
contention artifacts, not rendering-cost signal (helmet lavapipe:
min=6.691, **max=30.188**, 4.5x; Sponza lavapipe: min=30.115,
**max=345.496**, 11.5x). The report explicitly caveats similar variance
in the adjacent bake-timing table as "host-load/scheduling noise, not a
signal" but does **not** apply the same honest caveat to the frame-time
table, despite the frame-time table showing comparably or more extreme
spread. This strongly suggests the lavapipe frame-time measurement
session ran under real host contention (plausibly concurrent with the
same round's own packaging/Xephyr verification work) that was not
disclosed for this specific table.

**Why this matters:** these are the numbers the ledger will carry forward
as the Stage 1 performance floor; per CLAUDE.md, "a performance regression
blocks a phase exit the same way a failing test does" from Phase 4
onward. A baseline captured under undisclosed contention, on the CPU-bound
driver specifically, is not a safe floor to regress future rounds against.

**Fix direction:** re-measure the lavapipe numbers in a quiet, contention-
free session (mirroring this review's own methodology — confirm no
concurrent GPU-adjacent process via `nvidia-smi`/`ps` before measuring),
and either report median + a percentile (not just avg/min/max, given how
noise-prone this driver's timing is) or explicitly caveat the spread the
same way the bake-timing table already does. Combine with Finding 1's fix
so the republished numbers are both correctly-scoped and reproducible.

## Finding 3 (MINOR, pre-decided) — `samples/README.md` has no
`## 09_scene` section, and (newly found this round) no `## 07_stress`
section either

**Where:** `samples/README.md` — section headers run `01_triangle` →
`02_hotreload` → `03_bindless_mesh` → `04_streaming` → `05_multipass` →
`06_materials` → `08_gltf_viewer` → `Building and running` (confirmed via
`grep -n "^## " samples/README.md`).

**What's wrong:** Per this review's dispatch, the `09_scene` gap is a
pre-decided finding (coordinator adjudication overriding the
implementer's own "out of scope" call in task-12-report.md item 4) — the
brief's own Files row includes README rows, and this repo's no-deferral
policy closes discovered gaps in-round. Independently confirmed present:
`09_scene` has no dedicated walkthrough section (the file jumps straight
from `08_gltf_viewer` to `Building and running`).

**Additionally found this round, same defect class, not flagged by the
report:** `07_stress` **also** has no dedicated `## 07_stress` section —
it is referenced from other samples' prose and the directory-tree
manifest, but has no own walkthrough section either. Since this is the
identical structural gap the coordinator already ruled must close, it
should be swept in the same fix round rather than surfacing as a separate
discovery later.

**Fix direction:** add concise `## 07_stress` and `## 09_scene` sections
matching the existing per-sample format (What/expected output/CLI
flags/redistribution, as `06_materials`/`08_gltf_viewer` already do).

## Finding 4 — Engine-APIs-only audit and the exposure-label bug fix:
**verified correct, no defect**

Recorded for completeness since this was a named attention-lens item.
Independently confirmed, not merely re-read from the report:

- `grep -n "ImGui::\|rx::debug_ui::Overlay" samples/08_gltf_viewer/main.cpp
  | wc -l` → 19 (matches); `grep -n "FT_\|stb_truetype\|freetype"` → no
  output (matches: zero sample-local font/text-rendering hand-rolling).
- `rx_scene/include/rx_scene/camera.h` confirms
  `std::optional<float> exposureOverride = 1.0F;` — always engaged by
  default (header comment: "DEFAULTS ENGAGED, AT EXACTLY 1.0"), so reading
  `.has_value()` directly (the pre-fix bug) would indeed mislabel every
  unmodified run. The fix (`App::exposureOverrideFromCli`, a plain `bool`
  defaulted `false`, set `true` only in `applyExposureArg()` when
  `args.exposure != 0.0F`) is correctly wired and correctly read by
  `drawHud()`.
- **Independently reproduced both HUD states via my own offscreen Xvfb
  screenshots** (not Xephyr — no on-desktop visible window; display `:91`,
  1280x720, torn down after use), real NVIDIA RTX 2080, `--present
  --validate`:
  - Default args: HUD reads `intensity (physical, pre-exposure): 1.000`,
    `ev100: 14.966  (neutral default)`; real IBL-lit render with a visible
    sky/ground-gradient skybox.
  - `--exposure 5 --env-intensity 2.0`: HUD reads `intensity: 2.000`,
    `ev100: 14.966  (direct EV100 override engaged, --exposure)`; both
    skybox and helmet visibly darker than the default run (correct EV100
    direction). Zero `[error]` lines in either run's log; both windows
    closed cleanly.
- Packaging fixes independently re-proven end to end: built both zips via
  `tools/package_samples.sh`, unzipped Linux zip outside the build tree,
  ran all nine samples standalone on lavapipe — 9/9 `exit=0`/gate PASSED;
  `08`/`09` confirmed baking their environment from the **unzipped
  copy's own path** (not a dev-tree fallback). Deleted `brdf.slang` from
  the unzipped `08_gltf_viewer/material_shaders/` — reproduces a loud
  `MaterialSystem::create failed` (via `energy_compensation_off.slang`
  failing to compile its `import brdf` — same ultimate failure mode the
  report describes, a more precise trigger than the report's own probe but
  the same discrimination outcome); restored, re-verified clean pass.
  **Went beyond the report's own scoped-down Windows check**: built the
  Windows zip, unzipped it, and ran the `.exe` live under Wine (offscreen
  Xvfb, not visible on the desktop) rather than relying on the report's
  structural `unzip -l` listing alone — it passed cleanly, environment
  baked and bound from the packaged copy, `headless gate PASSED`. See the
  Windows-zip adjudication below.
- Task 5 audit row (report's own table, task-12-report.md) independently
  spot-checked against the actual code for environment binding, exposure,
  and HUD rendering — all three rows hold: display-only sample-side state,
  every actual computation/rendering routed through the named engine API.
  The `--bench-frames` row's "not a gap, same precedent as
  07/09's own un-promoted loops" framing is true for the *code-reuse*
  question (no reimplementation of engine internals) but not for the
  *methodology* question — see Finding 1, which is a distinct problem
  from the audit row's own scope.

---

## Benchmark-methodology adjudication

Warmup: implicit and adequate — pipelines are already built by the two
D17-gated real-draw frames (loading-state capture draws no scene, but
loaded-scene capture does) plus the HUD smoke-test frame, all of which run
*before* the `--bench-frames` loop starts; `graph.compile()` runs once,
not per iteration (confirmed: `compile()` call site is single, at
`runHeadless()`'s setup, `captureFrame()`'s closure never recompiles).
Fixed camera path: confirmed — view/proj and draw data are computed once
before the loaded-scene capture and never touched again inside the bench
loop, so every iteration renders an identical scene state. Full pipeline
coverage: confirmed — `recordForward()` draws real scene geometry plus a
conditional skybox draw (same render-graph "forward" pass), followed by
`recordTonemapDraw()`; IBL/BRDF are consumed by `standard_pbr.slang`
through the same bindless textures baked earlier — this genuinely
measures IBL + skybox + tonemap, not a stripped-down path. **The
methodology's real defect is scope, not coverage**: it measures render +
a full CPU/GPU readback round-trip as one undifferentiated number and
misrepresents that as matching an established convention that explicitly
excludes exactly that overhead (Finding 1). Percentiles: not published
(avg/min/max only) — given the demonstrated noise sensitivity on lavapipe
(Finding 2), a percentile or median would have been more honest than a
single average next to a wildly larger max.

## Windows-zip verification-scope adjudication

The report scoped this down to a structural `unzip -l` content check,
judging a live Wine run "disproportionate" given the packaging logic is
"platform-identical shell-script staging, already proven correct on
Linux." That reasoning is sound as far as it goes, and RC7's "prefer
offscreen/headless... as short as honest measurement allows" steer
supports not over-investing here. But the check was cheap enough that this
review simply did it: built the Windows zip, unzipped it, and ran
`sample_08_gltf_viewer.exe` live under Wine, offscreen (Xvfb, no
on-desktop window) — total added time under two minutes, zero setup beyond
what the Wine CI tier already requires. It passed cleanly (environment
baked and bound from the packaged copy's own path; `headless gate
PASSED`; the one pixel-gate "fail" line is Wine/wined3d's own informational-
only non-lavapipe note, matching this codebase's established convention
of not enforcing D17 pixel-identity off the reference driver). **Verdict:
the scope-down was a defensible call given the cost/benefit at the time,
but not a necessary one** — the check was inexpensive enough that it
should probably just be routine for this class of packaging fix rather
than argued down each time. No defect found either way; this review's own
live run closes the gap the report left open.

## Checkpoint completeness vs. the Phase 4 pattern

| Criterion | Status |
|---|---|
| Suite green, both presets | lavapipe 42/42 ✓; NVIDIA sample_08 3/3 ✓; Wine 14/14 ✓ (all independently re-run this round) |
| Real driver | ✓ NVIDIA GeForce RTX 2080, driver 580.82.07 |
| Packaged + standalone-verified | ✓ (independently re-proven end to end, Linux 9/9 + a live Wine run the report itself skipped) |
| Numbers driver-labeled | ✓ present, but see Findings 1-2 for correctness/reproducibility gaps |
| Deck rows tracked-not-run per RC8 | ✓ honestly stated, matches MANUAL_VERIFICATION.md's established pattern |

## Not independently verifiable this round

- The report's present-mode screenshots (`sample08_present_screenshot{2,3,4}...png`)
  live in the implementer's own session scratchpad, not committed — not
  accessible to this review. Not load-bearing: this review captured its
  own independent screenshots reproducing the same two states (Finding 4).
- Exact host-load conditions at the time of the report's own lavapipe
  measurement session could not be reconstructed after the fact (Finding
  2 is inferred from the reproducibility gap and the report's own
  internal inconsistency, not a direct measurement of the implementer's
  host state during their session).

## Hygiene

- Single commit `365a187` on `task/t12-stage1-exit`, cleanly on top of
  base `92eae34`.
- Author/committer: `Yousef Wadi <ywadi85@gmail.com>` (matches project
  git config). No AI attribution anywhere in the commit message or diff
  (`git show 365a187 | grep -iE "claude|anthropic|co-authored|generated
  with|ai assistant"` → no match).
- Not pushed: only a local `task/t12-stage1-exit` branch; `git log
  origin/main..HEAD` shows the same local history, no remote branch.
- Main checkout untouched (only the pre-existing `progress.md`
  modification noted at session start, left alone as instructed).
- Worktree restored byte-identical after this review's temporary
  instrumentation patch (`samples/08_gltf_viewer/main.cpp` — confirmed via
  `md5sum` match to the pre-edit copy and a clean `git status`/`git diff`
  afterward); the sample_08 binary was rebuilt from the restored source
  before the final confirmation test pass. Scratch unzip copies (including
  the deliberately-deleted `brdf.slang`/`ibl_shaders`/`environments` used
  for the discrimination probes) lived entirely under this session's
  scratchpad, outside the repository, and were removed after use — noted
  here per instructions, not restored (nothing in-repo to restore).

---

# Re-review (fix round 1)

Scoped re-review, commit `4d52d8f` (`fix(samples/08_gltf_viewer): review
fix round 1 -- correct --bench-frames methodology, add missing README
sections (#48)`), stacked on `365a187`, same branch/worktree. Scope: only
whether the three findings above are closed — no broader re-review.
`task-12-report.md`'s "Fix round 1" section read in full first, then every
claim independently re-verified in the worktree (`cd -P`, `nice -n 10`,
offscreen only — Xvfb, no on-desktop windows — solo GPU confirmed via
`nvidia-smi --query-compute-apps`/`ps aux` before each measurement).

## Overall verdict: **ALL ADDRESSED**

All three findings from the original review close cleanly, verified with
independent evidence, not merely re-read from the report. No new defects
found within this re-review's scope.

## Finding 1 (MAJOR, methodology) — **CLOSED**

`captureFrame()` (`samples/08_gltf_viewer/main.cpp`) now takes an optional
`double* recordMsOut`, timed around only `executor->execute()` inside the
first `runOnce()` call. Directly diffed against `samples/09_scene/main.cpp`'s
own `captureFrame()` (not just structurally compared): the timed region —
`recordStart` placement, the `if (recordMsOut != nullptr)` guard, the
`steady_clock`/`duration<double, milli>` computation — is byte-for-byte
identical to 09_scene's own pattern; only line-wrapping differs. The
`--bench-frames` loop now logs two separately-labeled metric families in
one `sample08: perf frame_bench` line:
`cpu_record_{avg,min,p95,max}_ms` (headline, comparable to 07/09) and
`full_{avg,min,p95,max}_ms` (the old whole-call number, kept as an
explicitly-separate readback-inclusive figure). p95 is nearest-rank
(`ceil(0.95*N)`, 1-based, clamped) on a sorted copy — confirmed in the
diff, does not mutate the insertion-order sample vectors. `Args::benchFrames`'
own doc comment documents both windows precisely (this sample has no
`--help` text; the flag's doc comment is the closest equivalent, as the
report states). Smoke-tested directly:

```
$ ./sample_08_gltf_viewer --validate --bench-frames 10
sample08: perf frame_bench ... cpu_record_avg_ms=0.217 cpu_record_min_ms=0.195 cpu_record_p95_ms=0.272 cpu_record_max_ms=0.272 full_avg_ms=1.764 full_min_ms=1.644 full_p95_ms=2.422 full_max_ms=2.422
```

## Finding 2 (MAJOR, reproducibility) — **CLOSED**

Re-ran helmet + Sponza on both drivers myself (quiet host confirmed:
`uptime` load average 1.04/0.74/0.77 on 8 cores, `nvidia-smi
--query-compute-apps` empty, no concurrent builds; `nice -n 10`, offscreen
Xvfb, same iteration counts as the original round — 60/scene lavapipe,
200/scene NVIDIA). Two independent runs per cell; both listed. All deltas
against the corrected table are far inside the 20% tolerance:

| Scene | Driver | Metric | Corrected table | Reviewer (run 1 / run 2) | Delta |
|---|---|---|---|---|---|
| DamagedHelmet | lavapipe | cpu_record avg | 0.253 ms | 0.267 / 0.264 ms | +5.5% / +4.3% |
| DamagedHelmet | lavapipe | full avg | 4.598 ms | 4.755 / 4.676 ms | +3.4% / +1.7% |
| DamagedHelmet | NVIDIA RTX 2080 | cpu_record avg | 0.219 ms | 0.208 ms | -5.0% |
| DamagedHelmet | NVIDIA RTX 2080 | full avg | 1.715 ms | 1.665 ms | -2.9% |
| Sponza | lavapipe | cpu_record avg | 4.644 ms | 4.681 / 4.478 ms | +0.8% / -3.6% |
| Sponza | lavapipe | full avg | 30.463 ms | 31.034 / 30.459 ms | +1.9% / 0.0% |
| Sponza | NVIDIA RTX 2080 | cpu_record avg | 4.547 ms | 4.298 ms | -5.5% |
| Sponza | NVIDIA RTX 2080 | full avg | 7.197 ms | 6.808 ms | -5.4% |

Largest observed delta is 5.5%, well under the 20% flag threshold, and
run-to-run variance on my own quiet host is itself in the same ~1-5% band
— consistent with genuine measurement noise, not a systematic
discrepancy. The original round's root-cause claim (concurrent
`windows-cross-zig` build + Wine `ctest` inflating the lavapipe numbers)
is plausible and consistent with what this review's own original probe
found (NVIDIA numbers matched cleanly then too; only the CPU-bound
lavapipe path was skewed) — not independently re-provable after the fact,
but the corrected numbers themselves reproduce, which is what this
finding required.

## Finding 3 (MINOR, README) — **CLOSED**

`grep -n "^## " samples/README.md` now shows `07_stress` (line 766) and
`09_scene` (line 1010) between `06_materials`/`08_gltf_viewer` and
`08_gltf_viewer`/`Building and running` respectively, in the correct
reading order. Both sections read in full: same shape as every sibling
section (prose intro, bulleted CLI-flag list, `### Expected output`,
`### Redistribution`) — no format drift. The top-of-file directory-tree
listing (the "Downloading a prebuilt sample bundle" section) now includes
a `09_scene/` entry (previously absent) and lists `06_materials`'/
`08_gltf_viewer`'s `brdf.slang`/`energy_compensation_{off,on}.slang`
files, matching their prose Redistribution sections. The stale "the other
seven" sample count is now "the other eight" (9 samples total minus
`01_triangle`'s own precompiled-SPV exception = 8 that do real in-process
Slang compilation — correct). `09_scene`'s own new Redistribution
paragraph additionally, accurately cites this review's own live-Wine
verification ("independently re-verified by this round's review,
`windows-cross-zig` under a live Wine execution") — no overclaim.

## Item 4 — Windows-zip packaging row citation — **accurate, no overclaim**

`task-12-report.md`'s Verification section (the "[Updated, Fix round 1]"
paragraph) correctly states what this review actually did and found: built
the Windows zip, unzipped it, ran the `.exe` live under Wine offscreen, it
passed cleanly, environment baked/bound from the packaged copy, and the
one pixel-gate "fail" line is Wine's own informational-only non-lavapipe
note (matching established convention, not a real failure). It correctly
preserves this review's own adjudication language ("the scope-down was a
defensible call... but the check was cheap enough... should just be
routine going forward") rather than overstating it as "the original
implementer's Windows check was wrong" or understating it as "unverified."
No overclaim in either direction.

## Commit hygiene (`4d52d8f`)

- Author/committer: `Yousef Wadi <ywadi85@gmail.com>` (matches). No AI
  attribution (`git show 4d52d8f | grep -iE "claude|anthropic|co-authored|
  generated with|ai assistant"` → no match).
- Pathspecs: exactly the two expected files —
  `samples/08_gltf_viewer/main.cpp` (175 insertions / 53 deletions across
  both commits combined; this commit's own diff is scoped to the
  `captureFrame`/`Args::benchFrames`/bench-loop region only) and
  `samples/README.md` (the two new sections + tree/count fixes). No
  stray files.
- Not pushed: only a local `task/t12-stage1-exit` branch; no remote
  branch for this ticket exists.
- Stacked cleanly on `365a187` (`git log --oneline -5` confirms linear
  history, no rebase/rewrite of the reviewed commit).

## Re-verification (this round)

- Full `ctest` (42 tests), `linux-native`, lavapipe: 42/42 passed.
- `sample_08_gltf_viewer_{headless,quit_during_load,exr_env_headless}`,
  real NVIDIA RTX 2080 (580.82.07): 3/3 passed.
- Both build presets confirmed up to date (`windows-cross-zig` also
  rebuilds and links `sample_08_gltf_viewer.exe` cleanly against the fix).
- No temporary edits were needed this round (the split-metric fix was
  already native code, not something this review had to instrument) — worktree
  and main checkout both confirmed clean (`git status --porcelain`) before
  and after; no stray processes left running.
