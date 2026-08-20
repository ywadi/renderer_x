# Independent review: Sponza-crash P0 round (`9e6a3da..ec86e87`)

Reviewer: independent (not the implementer). Repo:
`/media/ywadi/second/renderer_x` (real path only -- every command below ran
from that path, never the `/home/ywadi/d2/renderer_x` symlink alias).
Reviewed commits (5, on top of `9e6a3da`):

- `5f031bd` fix(samples,rx_rhi_vk): size 09_scene material-params pool from
  real material count (P0)
- `c3b6d0b` chore(tools): temporarily bundle fetched Sponza with the
  09_scene package
- `fe9042f` docs: Sponza crash fix report -- root cause, fix rationale,
  revert evidence
- `e86202c` fix(samples): 08_gltf_viewer material-params pool -- same-class
  DescriptorArena fix as 09
- `ec86e87` docs: append 08_gltf_viewer in-round closure to the Sponza
  crash fix report

Inputs read in full: `sponza-crash-fix-report.md`,
`review-9e6a3da..ec86e87.diff`.

**Driver discipline applied throughout this review**: every empirical claim
below is labeled with its driver. All "NVIDIA" runs used the **default
Vulkan loader ICD resolution -- `VK_ICD_FILENAMES` was never set** (unset
explicitly with `env -u VK_ICD_FILENAMES` before every such run). The
loader enumerated two devices on this machine (NVIDIA GeForce RTX 2080,
driver 580.82.07; llvmpipe/Mesa) and `vkb::PhysicalDeviceSelector`'s default
discrete-GPU preference selected the NVIDIA device -- independently
confirmed below by checking `nvidia-smi`'s own process list while the
sample ran, not merely inferred from success. Lavapipe runs explicitly
pinned `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`, labeled as
such -- pinning to force lavapipe (rather than to fake an NVIDIA run) is
not what the standing rule prohibits.

---

## Verdicts

**Spec compliance: PASS.** The round's mandate -- route sample 09's (and
now 08's) material-params allocation through `rx::rhi::DescriptorArena`
sized from the real per-run material count, with no magic numbers left, so
over-demand becomes a clean logged failure on any driver -- is met exactly
as described. Verified independently at the code level (both call sites,
all six call sites across the two samples) and empirically (real NVIDIA
Sponza run, real NVIDIA sample_08 run, full lavapipe ctest, packaging
round-trip, and a from-scratch reproduction of both the original crash and
the regression test's discrimination power).

**Code quality: Approved**, with one Nit (non-blocking) noted below. No
Major or Critical findings. Commit hygiene is clean.

---

## 1. Fix shape -- `DescriptorArena`, sized from real demand

Read both samples' full call sites (`samples/09_scene/main.cpp`,
`samples/08_gltf_viewer/main.cpp`) against the current, committed tree (not
just the diff):

- `grep` for `kMaxMaterialParamSets` / `materialParamPool` /
  `vkCreateDescriptorPool` / `vkAllocateDescriptorSets` in both files
  returns **zero live hits** -- only historical mentions inside comments
  explaining what was removed and why. No magic number survived anywhere
  reachable at runtime.
- `App::materialParamPool` (`VkDescriptorPool`) became
  `App::materialParamArena` (`std::optional<rx::rhi::DescriptorArena>`) in
  both samples, exactly as the report describes.
- `createMaterialParamArena(App&, uint32_t materialCount)` exists in both
  files, clamps `materialCount` to a minimum of 1
  (`std::max<uint32_t>(materialCount, 1)`), and is called from **every**
  path that can reach `finalizeMaterialBinding()`/`setupMaterials()`:
  - 09_scene: 5 call sites (`runHeadless` stress/grid, `runPresent`
    stress/scene/grid) -- confirmed by line number against the diff, each
    passing the correct count (`kStressVariantCount`, `1`, or
    `result.materials.size()`).
  - 08_gltf_viewer: 2 call sites (`runHeadless`'s and `runPresent`'s async
    import completion callbacks), both immediately before their respective
    `setupMaterials()` call, both passing `result.materials.size()`.
- Read `src/rx_rhi_vk/include/rx_rhi_vk/descriptor_arena.h` and
  `src/rx_rhi_vk/src/descriptor_arena.cpp` in full: `allocate()` checks
  `allocatedSets_[currentFrame_] + 1 > capacities_.maxSets` and the
  per-type UBO budget **before** ever calling `vkAllocateDescriptorSets`,
  returning a logged `VK_NULL_HANDLE` on either ceiling. This check is
  arena-side software accounting, not driver-dependent -- it is the "clean
  logged failure on any driver" guard contract the round's mandate
  requires, and it predates this round (used previously only by
  `rx_material::ParamArena`); this round is reuse, not new invention,
  consistent with the repo's "don't reinvent the wheel" rule.
- `destroyApp()` in both samples now just `.reset()`s the
  `std::optional<DescriptorArena>`; confirmed the class's move-assignment
  and destructor both call `destroyAll()` (which iterates `pools_` and
  calls `vkDestroyDescriptorPool`), so this is a behavior-preserving
  replacement for the old explicit `vkDestroyDescriptorPool` call, and
  `reset()` on a never-populated optional (an early-failure path before any
  mode was selected) is a safe no-op.

**Conclusion**: fix shape matches the report's description exactly, no
discrepancy between the report's narrative and the actual committed code.

## 2. Regression test discrimination -- reproved independently

Read the new `TEST_CASE` in `src/rx_rhi_vk/tests/descriptor_arena_test.cpp`
("... discriminates Sponza-scale demand against an undersized pool ...").
It pins the incident's two real numbers (`kPreFixSampleCapacity = 8`,
`kSponzaMaterialCount = 25`) directly against `DescriptorArena`'s own
arena-enforced accounting, in two nested blocks (undersized-pool /
correctly-sized-pool), consistent with the report.

**Revert-proved myself** (not trusted from the report):

1. Confirmed tree was clean before starting (`git diff --stat HEAD` showed
   only the pre-existing `progress.md` line, unrelated to this file).
2. Edited `src/rx_rhi_vk/src/descriptor_arena.cpp`, prefixing both budget
   `if` conditions in `allocate()` with `false &&` to neuter the
   arena-enforced check.
3. Rebuilt `rx_rhi_vk_tests` (linux-native), ran only the new test case
   under **lavapipe** (`VK_ICD_FILENAMES=lvp_icd.json`, `xvfb-run`):
   ```
   /media/ywadi/second/renderer_x/src/rx_rhi_vk/tests/descriptor_arena_test.cpp:318:
   ERROR: CHECK( succeeded == kPreFixSampleCapacity ) is NOT correct!
     values: CHECK( 25 == 8 )
   [doctest] test cases:  1 |  0 passed | 1 failed | 71 skipped
   ```
   -- bit-for-bit the same failure the report claims, reproduced
   independently, not copy-pasted from it.
4. Restored via `git checkout HEAD -- src/rx_rhi_vk/src/descriptor_arena.cpp`;
   confirmed `git diff --stat HEAD` for that file was empty afterward.
   Rebuilt again; the same test now passes (**lavapipe: 33/33
   assertions**).

**Conclusion**: the test genuinely discriminates -- it is not vacuous, and
the revert-restore round-trip left the tree byte-identical to the reviewed
commit.

## 3. Empirical verification (driver-labeled)

### 3a. Real NVIDIA, default loader ICD (no `VK_ICD_FILENAMES`) -- GeForce RTX 2080, driver 580.82.07

Confirmed device selection independently: launched the sample in the
background and cross-checked `nvidia-smi --query-compute-apps` /
`nvidia-smi pmon`, which listed `sample_09_scene`'s own PID as a live
`C+G` client on GPU0 (610 MiB) -- not inferred from log success alone.

- `sample_09_scene --present --scene <bundled Sponza path>` (no
  `--validate`, `timeout 25`): loaded cleanly (`material-params descriptor
  arena sized for 25 material(s)`, `... loaded -- 1 renderable(s), 25
  material(s)`), ran ~22.3s live (load-complete to pipeline-cache-save),
  zero `[error]` lines, `window closed cleanly`.
- Same command **+ `--validate`** (Khronos validation layer active,
  `timeout 25`): ~22.2s of live rendering, **0 unfiltered `[error]` lines**
  (`grep -ci "\[error\]"` on the full log -> 0; every "Validation Error:"
  string present is logged at `[warning]` level with an explicit
  "known false positive" annotation, matching the report's description of
  a pre-existing, already-documented sync-validation misclassification),
  clean `window closed cleanly` exit. Satisfies the "sustained >=20s, zero
  unfiltered validation errors, clean exit" requirement.
- `sample_08_gltf_viewer --present --validate` (default DamagedHelmet,
  `timeout 15`): `material-params descriptor arena sized for 1
  material(s)`, scene loaded, **0 unfiltered `[error]` lines**, clean
  `window closed cleanly` exit.

### 3b. Revert-discrimination A -- reproduced the original crash myself

Independently of the test-level revert above: swapped
`samples/09_scene/main.cpp` for its pre-fix content
(`git show 9e6a3da:samples/09_scene/main.cpp`), rebuilt, ran the exact
crash-report command against the bundled Sponza path on the **real NVIDIA
GPU, default loader ICD**:

```
[error] sample_09_scene: vkAllocateDescriptorSets (material params) failed
```

exit code 1 -- reproduces the reported symptom exactly. Restored via
`git checkout HEAD -- samples/09_scene/main.cpp` (`git diff --stat HEAD` for
that file empty afterward), rebuilt again, confirmed the fix is back
(`material-params descriptor arena sized for 25 material(s)`, clean load,
no error).

### 3c. Lavapipe, explicitly pinned -- full serial ctest

`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`, `xvfb-run -a`,
real repo path:

```
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  73.90 sec
```

**29/29**, run once as required, from the final (fix-restored) tree state.
Also re-ran `sample_08`/`sample_09`'s five ctest gates in isolation
(same lavapipe label): all five pass.

### 3d. `rx_rhi_vk_tests` full binary, both drivers

- Lavapipe (`xvfb-run`, pinned): **72/72 test cases, 2141/2141 assertions,
  0 failed.** (The report claims 2153/2153 for lavapipe; my count is 2141.
  Both are 100%-pass with 0 failures -- the discrepancy is an
  environment-dependent assertion-count artifact, not a correctness gap: a
  logged `window_state_test.cpp` message in my run explicitly states this
  xvfb/WM combination skips a block of resize-parity assertions that a
  different windowing environment would execute. Flagging as a minor,
  non-blocking observation rather than a defect -- see Findings.)
- Real NVIDIA, default loader ICD: **72/72 test cases, 2164/2164
  assertions, 0 failed** -- matches the report's own NVIDIA number exactly.

### 3e. windows-cross-zig

Rebuilt `sample_09_scene`, `sample_08_gltf_viewer`, `rx_rhi_vk_tests` for
the `windows-cross-zig` preset from the current tree: clean build, all
three link successfully (`.exe` outputs produced). Not run (cross-compiled,
no Windows runtime available here) -- build-only verification, consistent
with what the report itself claims for this preset.

## 4. Packaging (`c3b6d0b`)

Read the full diff to `tools/package_samples.sh`: the Sponza staging block
is bounded by clear `# ====` markers, explicitly commented TEMPORARY, uses
`SPONZA_SRC="$REPO_ROOT/assets/fetched/Sponza/glTF"` with a directory-
existence guard that `echo`s the exact fetch instruction and `exit 1`s if
absent -- combined with `set -euo pipefail` (confirmed present near the top
of the script), this is a loud, non-silent failure path by direct code
inspection. (I did not empirically trigger this path by moving the real
`assets/fetched/Sponza` directory aside -- that would have touched shared
state outside this diff's scope on a checkout other agents/tasks may be
using concurrently, and the harness declined the destructive `mv` for that
reason. The failure path is simple, linear bash with no branching I could
not already trace by reading, so I'm treating it as verified-by-inspection
rather than empirically re-proven, and flagging that distinction here
rather than silently overclaiming.)

Ran the real packaging script:

```
$ bash tools/package_samples.sh linux-native linux-x86_64 <zip>
```

-- staged 71 files under `09_scene/assets/Sponza/glTF/` (one `.gltf`, one
`.bin`, 69 textures) plus `09_scene/assets/Sponza/PROVENANCE.txt`
(contents match the report's citation: KhronosGroup source, CRYENGINE
Limited License Agreement). `STAGE_DIR` is a `mktemp -d` (confirmed by
reading the script) -- no repo-tree pollution; `git status --short`
confirmed clean (only the pre-existing `progress.md` line) immediately
after the packaging run.

Unzipped **outside the repo**
(`/tmp/claude-1000/.../scratchpad/pkg_unzip/`, well outside
`/media/ywadi/second/renderer_x`; confirmed via `realpath` that the
extraction directory is not a repo subpath). Ran the packaged binary from
inside the extracted `09_scene/` directory against the **bundled, relative**
path, real NVIDIA GPU, default loader ICD:

```
$ ./sample_09_scene --present --scene assets/Sponza/glTF/Sponza.gltf
[info] sample_09_scene: material-params descriptor arena sized for 25 material(s)
[info] sample_09_scene: 'assets/Sponza/glTF/Sponza.gltf' loaded -- 1 renderable(s), 25 material(s)
...
[info] sample_09_scene: window closed cleanly
```

Zero `[error]` lines, clean exit. Matches the report's own packaging
verification exactly.

## 5. Commit hygiene

- 5 commits, exactly as listed in the diff package header.
- Pathspec scope confirmed per-commit via `git show --name-only`:
  `5f031bd` -> `samples/09_scene/main.cpp` +
  `src/rx_rhi_vk/tests/descriptor_arena_test.cpp`; `c3b6d0b` ->
  `tools/package_samples.sh` only; `fe9042f`/`ec86e87` -> the report `.md`
  only; `e86202c` -> `samples/08_gltf_viewer/main.cpp` only. No commit
  touches anything outside its stated scope.
- Author on every commit: `Yousef Wadi <ywadi85@gmail.com>` -- matches
  local `git config user.name`/`user.email` exactly. No AI attribution of
  any kind found in any commit message (checked full `%B` bodies, not just
  subjects).
- `git log origin/main..HEAD` shows all 5 commits as unpushed;
  `git branch -vv` confirms `main` is `[origin/main: ahead 5]`. Nothing
  pushed.
- Working tree: only the pre-existing, not-this-round `progress.md`
  modification remains dirty throughout, exactly as the report states and
  as instructed to leave alone. No other stray changes at any point during
  this review (verified `git status --short` before, during -- after each
  temporary revert-experiment restore -- and after this review's own
  empirical work).

## Findings

- **Nit (non-blocking): `createMaterialParamArena()` is duplicated
  near-verbatim between `samples/08_gltf_viewer/main.cpp` and
  `samples/09_scene/main.cpp`** (same body, differing only in log-message
  sample-name prefix and the caller-side count expressions). Each sample in
  this codebase is already an intentionally self-contained binary (both
  samples' own comments say so explicitly, and this predates this round --
  the two samples already had separately hand-rolled, structurally
  identical `VkDescriptorPool` blocks before this fix), so this is
  consistent with existing project convention rather than a new violation;
  not a "reinvent the wheel" violation either, since both call sites route
  through the one shared `rx::rhi::DescriptorArena` primitive -- only the
  ~10-line adapter around it is duplicated. Worth a follow-up extraction
  (e.g. a shared samples-support header) if a third sample ever needs the
  same pattern, but not worth blocking this P0 round on.
- **Observation (non-blocking): lavapipe `rx_rhi_vk_tests` assertion count
  differs between this review's run (2141/2141) and the report's claimed
  run (2153/2153).** Both are 100%-pass, 0-failed, 72/72 test cases; a
  `window_state_test.cpp` message in my run explicitly attributes the gap
  to this xvfb/WM combination skipping a block of resize-parity assertions
  a different windowing environment executes. Not a correctness
  regression -- flagging only because the standing rule demands verified
  numbers, not asserted ones, and my number genuinely differs from the
  report's.
- **Not empirically re-verified: packaging's "fails loudly without the
  fetch" path.** Verified by direct code inspection only (simple, linear
  `set -euo pipefail` bash with an explicit directory-existence guard and
  instructive `exit 1` message) -- the live destructive test (temporarily
  moving `assets/fetched/Sponza` aside) was not performed because it would
  have mutated shared repo-adjacent state outside this diff's scope while
  other work may be running against the same checkout, and the harness
  declined the `mv` for that reason.
- No Major or Critical findings. Fix shape, regression coverage, empirical
  behavior on both drivers, packaging behavior, and commit hygiene all
  independently reproduce the report's claims.

## Restoration

All temporary edits made during this review (`descriptor_arena.cpp`'s
neutered budget check, `samples/09_scene/main.cpp`'s pre-fix swap) were
restored via `git checkout HEAD -- <file>` and confirmed byte-identical to
the committed tree (`git diff --stat HEAD` empty for each) before rebuilding
the final, fixed binaries used for the empirical verification recorded
above. The pre-existing `.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md`
modification was left untouched throughout, per instruction. Scratch
artifacts (logs, the packaged-zip round-trip, the pre-fix source copy) live
under this review's own scratchpad directory, outside the repo, and were
never committed.
