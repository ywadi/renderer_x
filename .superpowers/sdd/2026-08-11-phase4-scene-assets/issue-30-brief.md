# Issue #30 fix brief — rx_asset_gltf_gpu_tests determinism

Repo `/media/ywadi/second/renderer_x`, main at abd3d94 (helmet chain
merged; CI for it in flight — irrelevant to you). You are closing
GitHub issue #30: the pre-existing wall-clock/iteration-budget flake
in `rx_asset_gltf_gpu_tests`. Per project policy this closes now, not
"when it next recurs".

## The defect

`rx_asset_gltf_gpu_tests` shows run-to-run VARIANCE in its assertion
count: streaming/iteration-budget test cases derive how many
iterations/assertions execute from live wall-clock timing. On slow or
noisy hosts (CI: 2-core + lavapipe) this has tripped thresholds
outright in the past; today it "passes green" with a different number
of assertions each run — which means some runs exercise fewer checks
than others, i.e. coverage is nondeterministic.

History you must read first:
- `gh issue view 30` — the ticket with binding acceptance criteria.
- Ledger `.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md`
  — grep for "wall-clock", "self-calibrating", "flake": the segfault
  chain added a self-calibrating threshold (max(2ms, 4× live probe))
  which stopped the CI misfires; the variance remained.
- The test file(s): find the streaming/iteration-budget cases in
  `src/rx_asset/tests/` (grep for the calibration/threshold code and
  for loops keyed on elapsed time).

## Required outcome (acceptance criteria from #30, binding)

1. Streaming/iteration-budget cases assert on DETERMINISTIC quantities
   (event counts, state transitions, explicit tick/pump counts) rather
   than quantities derived from live wall-clock timing — or the timing
   dependence is bounded so assertion counts are stable across runs.
2. Proof: 10 consecutive serial runs under throttling
   (`taskset -c 0,1`) produce IDENTICAL assertion counts. Paste all 10
   count lines.
3. NO weakening of load-bearing claims (staleness detection, budget
   enforcement, eviction ordering) — the same defect classes must
   still be discriminated. Where a wall-clock threshold currently
   guards a claim (e.g. the stall detector), keep the claim but make
   the ASSERTED quantity deterministic (e.g. drive the clock via a
   seam/fake where one exists, or assert on the detector's decision
   given a controlled input rather than on how many iterations
   happened to fit in a time slice).
4. Revert-style evidence that the reworked tests still discriminate:
   for at least the two most load-bearing claims, break the product
   behavior locally (scratch worktree — symlink `toolchain/` into it,
   repo convention, or in-tree revert-and-restore), show the test
   FAILS, restore, show it passes.
5. Full `rx_asset_gltf_gpu_tests` + serial linux ctest 22/22 +
   windows-cross-zig build green; zero unfiltered validation errors.

If you find a case whose timing dependence CANNOT be removed without a
missing product seam (a real prerequisite), do NOT bodge it: report
BLOCKED for that case with the named prerequisite and close the rest.

## Rules

Pathspec-scoped local commits, NO AI attribution, author = local git
config, no push, no board/issue/plan/spec/ledger edits, per-directory
style. An independent reviewer will re-run your 10-run determinism
proof.

## Report contract

Full report → `.superpowers/sdd/2026-08-11-phase4-scene-assets/issue-30-report.md`
(per-criterion evidence, the 10 count lines, revert evidence, deviations).
FINAL MESSAGE: ONLY status, commit SHAs, one-line test summary, concerns.
