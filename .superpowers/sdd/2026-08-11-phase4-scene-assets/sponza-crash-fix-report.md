# P0: `sample_09_scene --present --scene Sponza.gltf` NVIDIA crash -- fix + Sponza packaging

Repo: `/media/ywadi/second/renderer_x` (real path only -- every build/test/run
command in this report was executed from that path, never the
`/home/ywadi/d2/renderer_x` symlink alias). Started from `main` at `9e6a3da`.

Commits:
- `5f031bd` fix(samples,rx_rhi_vk): size 09_scene material-params pool from
  real material count (P0)
- `c3b6d0b` chore(tools): temporarily bundle fetched Sponza with the
  09_scene package

Tree stayed clean except the pre-existing, not-mine
`.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md` modification
that was already present before this task started.

## 1. Root cause

**Symptom** (verbatim, reproduced below): `sample_09_scene --present --scene
<Sponza.gltf>` prints

```
[error] sample_09_scene: vkAllocateDescriptorSets (material params) failed
```

on a real, limit-enforcing driver (NVIDIA RTX 2080, driver 580.82.07) about
half a second in, then fails out.

**Where**: `samples/09_scene/main.cpp`. `makeApp()` built ONE hand-rolled
`VkDescriptorPool` (`App::materialParamPool`) at startup, sized by a
`constexpr uint32_t kMaxMaterialParamSets = 8;` -- both `maxSets` and the
pool's single `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` pool-size entry used this
same constant. The comment at the call site said exactly what it was sized
for: *"Sized for helmetMaterial (1) + kStressVariantCount (4) -- generous
headroom either way since only one of the two is ever populated per run."*
That comment is correct for the grid (1 material) and `--stress` (4
materials) modes -- and simply never accounts for the THIRD mode,
`--present --scene <path>`, added later. `setupImportedMaterials()` (called
only from that third mode) loops over **every material the imported scene
reports** and calls `finalizeMaterialBinding()` once per material, each call
doing one `vkAllocateDescriptorSets` (1 set, 1 UBO descriptor) against that
same 8-set pool. The pool is allocated once, up front, and never reset --
unlike `rx_material`'s own per-frame `ParamArena`, these sets are static
material-definition data that must persist for the whole run.

**The exact demand-vs-capacity numbers**:

| | value |
|---|---|
| Pool capacity (pre-fix `kMaxMaterialParamSets`) | 8 sets / 8 UBO descriptors |
| Sponza's own material count (`assets/fetched/Sponza/glTF/Sponza.gltf`, verified via `python3 -m json`) | **25** materials |
| First failing call | material index 8 (the 9th `finalizeMaterialBinding()` call) |
| Shortfall | 17 sets short of the real demand |

**Why lavapipe never caught it**: `src/rx_rhi_vk/include/rx_rhi_vk/descriptor_arena.h`'s
own class-level comment already documents this exact gap for a DIFFERENT
pool in this codebase (`rx::rhi::DescriptorArena`, used by `rx_material`'s
`ParamArena`): the Vulkan spec never obligates a driver to detect or report
`VkDescriptorPool` exhaustion (`vkAllocateDescriptorSets` "may" fail on
over-allocation, not "must"), and lavapipe/Mesa is a real, spec-conformant
implementation that does not choose to. `samples/09_scene/main.cpp`'s own
hand-rolled pool had NO software-side accounting of its own (unlike
`DescriptorArena`), so it inherited that gap directly: every prior Sponza
verification of this sample ran under lavapipe, which silently accepted all
25 allocations against an 8-set pool, and only a real, limit-enforcing
driver ever surfaced the bug.

**Why this is a sample bug, not a library bug**: `rx::material::MaterialSystem`
does not own this pool at all -- `samples/08_gltf_viewer` and
`samples/09_scene` each build their own, separate, hand-rolled "set-1
material params" `VkDescriptorPool` directly in the sample, structurally
identical to (but a SEPARATE object from) the set-1 layout
`MaterialSystem::reflectMaterialLayout()` builds internally per material
(verified via the Vulkan 14.2.2 pipeline-layout-compatibility argument
`08_gltf_viewer/main.cpp`'s own comment already spells out). `08_gltf_viewer`
had already been bumped to `kMaxMaterialParamSets = 64` for the same reason
(comment: *"Sized generously ... every glTF asset this sample
ships/documents has far fewer materials than that"*) -- `09_scene` simply
never received the equivalent bump when its own `--scene <path>` mode was
added. `MaterialSystem` and `rx::rhi::DescriptorArena` themselves were
correct and untouched by the bug.

## 2. Fix

**Reused, did not reinvent**: this project's engineering rule is "prefer
ready-made libraries ... over writing subsystems from scratch." This
codebase already has exactly the right primitive for this job:
`rx::rhi::DescriptorArena` (`src/rx_rhi_vk/include/rx_rhi_vk/descriptor_arena.h`)
-- a sized, arena-enforced `VkDescriptorSet` allocator that tracks its own
declared capacity in software and refuses (`VK_NULL_HANDLE`, logged) BEFORE
ever calling `vkAllocateDescriptorSets` once that capacity would be
exceeded, deterministically on every driver including lavapipe. It was
previously used only by `rx_material::ParamArena`; `samples/09_scene/main.cpp`
now uses it directly for its own set-1 material-params pool instead of a
second, hand-rolled, unaccounted `VkDescriptorPool`.

**Sized from real demand, not a bigger magic number**: `App::materialParamPool`
(`VkDescriptorPool`) became `App::materialParamArena`
(`std::optional<rx::rhi::DescriptorArena>`). `makeApp()` still builds the
mode-independent `VkDescriptorSetLayout` up front, but no longer builds a
pool at all -- pool creation moved to a new
`createMaterialParamArena(App&, uint32_t materialCount)` (`main.cpp`, right
after `makeApp()`), called from `runHeadless()`/`runPresent()` once the
active mode -- and, for `--scene`, the REAL imported material count -- is
known, and always before the first `finalizeMaterialBinding()` call for
that mode:

| call site | `materialCount` passed |
|---|---|
| `--stress` (both headless and present) | `kStressVariantCount` (4) |
| default DamagedHelmet grid (both headless and present) | `1` (only `result.materials[0]` is ever bound) |
| `--present --scene <path>` (Sponza or any other custom import) | `static_cast<uint32_t>(result.materials.size())` -- the scene's own real count, known only after `importGltf()` returns |

`finalizeMaterialBinding()` now calls
`app.materialParamArena->allocate(app.materialParamSetLayout, 1)` instead of
a raw `vkAllocateDescriptorSets`; `destroyApp()` now just
`.reset()`s the `std::optional<DescriptorArena>` (its destructor destroys
the underlying pool(s), same effect as the old explicit
`vkDestroyDescriptorPool` call).

## 3. Regression coverage

Added one new `TEST_CASE` to `src/rx_rhi_vk/tests/descriptor_arena_test.cpp`
("... discriminates Sponza-scale demand against an undersized pool (8-set
capacity vs. 25-material demand) deterministically, even under lavapipe"),
pinning this incident's own two real numbers directly against
`DescriptorArena`'s arena-enforced accounting:

- An 8-capacity arena (the pre-fix sample constant) accepts EXACTLY 8 of 25
  allocation attempts and refuses the rest.
- A 25-capacity arena (what `createMaterialParamArena()` now derives for
  Sponza) accepts all 25 with zero rejections.

This is the "assertion/accounting check that FAILS on any driver when
demand would exceed the pool" requirement: the check already lives inside
`DescriptorArena::allocate()` (arena-side, before the driver call), and this
new test proves it discriminates on this exact incident's numbers.

### Revert-discrimination evidence (mandatory, scratch, never committed)

**A -- the original bug was real and is what got fixed.** With
`samples/09_scene/main.cpp` temporarily restored to its pre-fix content
(`git show 9e6a3da:samples/09_scene/main.cpp`, single file, rebuilt, then
restored via `git checkout HEAD --` and rebuilt again -- `git diff --stat
HEAD` empty before and after), the exact crash-report command against the
real NVIDIA driver reproduced the exact reported symptom:

```
[2026-08-20 08:23:39.234] [error] sample_09_scene: vkAllocateDescriptorSets (material params) failed
```

exit code 1. With the fix restored and rebuilt, the identical command runs
clean (section 4 below).

**B -- the new test genuinely discriminates, not vacuously.** With
`DescriptorArena::allocate()`'s two arena-enforced budget checks temporarily
neutered (`if (false && ...)`, `src/rx_rhi_vk/src/descriptor_arena.cpp`,
rebuilt, then restored via `git checkout HEAD --` and rebuilt again -- `git
diff --stat HEAD` empty before and after), the new test FAILED under
lavapipe exactly as expected:

```
/media/ywadi/second/renderer_x/src/rx_rhi_vk/tests/descriptor_arena_test.cpp:318: ERROR: CHECK( succeeded == kPreFixSampleCapacity ) is NOT correct!
  values: CHECK( 25 == 8 )
```

-- i.e. with the arena-side check disabled, lavapipe silently let all 25
allocations through an 8-set pool (the exact original failure mode,
reproduced at the mechanism level). With the real check restored, the same
test passes on both lavapipe and NVIDIA (section 4).

Both experiments used the real build system/binaries/drivers, restored via
`git checkout HEAD -- <file>` (non-destructive: both files were already
committed, `git diff --stat HEAD` confirmed empty before starting and after
finishing each experiment), then rebuilt to leave the tree's actual build
artifacts in the fixed state before final verification.

**Scope note on "revert-proof"**: the new test exercises
`rx::rhi::DescriptorArena`'s own arena-enforced mechanism directly (proven
above to genuinely discriminate), which is what the fix now routes through.
It does NOT call through `samples/09_scene/main.cpp` itself --
`sample_09_scene`'s ctest-registered headless gates
(`sample_09_scene_headless`, `sample_09_scene_stress_headless`) never
exercise the `--scene <path>` custom-import branch on any driver, before or
after this fix (only `--present --scene` does, which is interactive-only
and not ctest-registered). So a future regression in the SAMPLE's own
`createMaterialParamArena()` call sites (e.g. someone passing a wrong count)
would not be caught by `ctest` -- but it also could no longer reproduce a
crash: `DescriptorArena::allocate()`'s own arena-enforced refusal turns any
such misuse into a clean, loud `RX_LOG_ERROR` + `return false` (exit code 1)
on every driver, never a driver-dependent failure past a raw
`vkAllocateDescriptorSets` call. Flagging this as a known coverage gap
rather than overclaiming full end-to-end ctest coverage of the sample's own
sizing call sites.

## 4. Verification

### 4a. NVIDIA path (real GPU: GeForce RTX 2080, driver 580.82.07, real X11
session `:1`, `VK_ICD_FILENAMES` pinned to `/usr/share/vulkan/icd.d/nvidia_icd.json`)

Exact crash-report command, sustained run (SIGTERM after ~30s via `timeout
32`, caught by SDL and shut down cleanly -- not a crash):

```
$ timeout 32 ./build/linux-native/samples/09_scene/sample_09_scene --present \
    --scene /media/ywadi/second/renderer_x/assets/fetched/Sponza/glTF/Sponza.gltf
...
[2026-08-20 08:13:18.775] [info] sample_09_scene: material-params descriptor arena sized for 25 material(s)
[2026-08-20 08:13:20.130] [info] sample_09_scene: '/media/ywadi/second/renderer_x/assets/fetched/Sponza/glTF/Sponza.gltf' loaded -- 1 renderable(s), 25 material(s)
[2026-08-20 08:13:20.136] [info] rx_platform: gamepad connected id=2 name="Generic X-Box pad" ...
[2026-08-20 08:13:49.412] [info] rx_material: saved 59416 bytes of pipeline cache data to '.../scene_pipeline.cache'
[2026-08-20 08:13:49.579] [info] sample_09_scene: window closed cleanly
```

~29.4s of live rendering (08:13:20.136 load-complete to 08:13:49.412
shutdown-begin), **zero `[error]`/`[critical]`/fatal/segfault lines** in the
92-line log.

Same command + `--validate` (Khronos validation layer active), same
duration: **zero unfiltered validation errors** (`grep -i "\[error\]" | grep
-v "known false positive"` -> 0 matches; 30097 matches were the
already-documented, pre-existing "known false positive" sync-validation
misclassification `context.cpp` already filters -- unrelated to this fix,
present before and after), clean `window closed cleanly` exit.

### 4b. lavapipe (unaffected)

Full serial `ctest`, `VK_ICD_FILENAMES` pinned to
`/usr/share/vulkan/icd.d/lvp_icd.json`, `xvfb-run`, from the real repo path:

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
    ctest --test-dir /media/ywadi/second/renderer_x/build/linux-native --output-on-failure
...
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  75.24 sec
```

**29/29**, run a second time after the revert-evidence rebuilds in section
3 to confirm the final committed state is what's green. The new
`rx_rhi_vk_tests` regression test (part of `#6`) and both `sample_09_scene`
headless gates (`#27`/`#28`) are included and pass.

Full `rx_rhi_vk_tests` binary in isolation (not just the DescriptorArena
cases), both drivers: **72/72 test cases, 2153/2153 assertions (lavapipe)**,
**72/72 test cases, 2164/2164 assertions (NVIDIA)**.

### 4c. windows-cross-zig

```
$ cmake --build build/windows-cross-zig --target sample_09_scene rx_rhi_vk_tests -j"$(nproc)"
...
[4/4] Linking CXX executable samples/09_scene/sample_09_scene.exe
$ cmake --build build/windows-cross-zig -j"$(nproc)"
ninja: no work to do.
```

Clean build, targeted rebuild + a full incremental rebuild of the whole
preset (confirms nothing else in the dependency graph broke).

### 4d. Packaging (Task 2)

```
$ bash tools/package_samples.sh linux-native linux-x86_64 <zip>
...
09_scene/assets/Sponza/glTF/Sponza.gltf        (167176 bytes)
09_scene/assets/Sponza/glTF/Sponza.bin         (9528220 bytes)
+ 69 more files (textures) under 09_scene/assets/Sponza/glTF/
09_scene/assets/Sponza/PROVENANCE.txt          (153 bytes)
package_samples: done.
```

71 files staged under `09_scene/assets/Sponza/glTF/` (matches
`tools/fetch_assets.sh`'s own documented Sponza file count exactly), plus
`PROVENANCE.txt`. `LICENSE*`/`README*` glob for an upstream license file
found nothing to copy (`tools/fetch_assets.sh --sponza` does not itself
fetch one -- see `PROVENANCE.txt`'s own citation of the license instead).

Unzipped OUTSIDE the repo
(`/tmp/claude-1000/.../scratchpad/p0_pkg_verify*/`, well outside
`/media/ywadi/second/renderer_x`), ran the packaged binary against the
BUNDLED path from inside the extracted `09_scene/` directory:

```
$ ./sample_09_scene --present --scene assets/Sponza/glTF/Sponza.gltf
...
[info] sample_09_scene: material-params descriptor arena sized for 25 material(s)
[info] sample_09_scene: 'assets/Sponza/glTF/Sponza.gltf' loaded -- 1 renderable(s), 25 material(s)
...
[info] sample_09_scene: window closed cleanly
```

Zero `[error]`/fatal/segfault lines. Repeated once more after the final
re-package (post revert-evidence rebuilds) with the same clean result.

Packaged zip size: 425MB total (linux-native, all 9 samples); Sponza's own
footprint inside `09_scene/`: ~51MB (matches `tools/fetch_assets.sh`'s own
documented "~53 MB" estimate).

## 5. README note for the coordinator (not made by this task -- packaging
script does not own README.txt generation)

The 09_scene zip's README.txt (added by hand at release-packaging time, per
this repo's own convention) should now additionally document the bundled
Sponza path:

```
./sample_09_scene --present --scene assets/Sponza/glTF/Sponza.gltf
```

alongside the existing DamagedHelmet-default documentation, and should note
that this bundling is TEMPORARY (owner directive, "for now") and licensed
under the CRYENGINE Limited License Agreement (see
`assets/Sponza/PROVENANCE.txt`), not Creative Commons like DamagedHelmet.

## 6. Deviations / concerns

- **`samples/08_gltf_viewer` shared the same architectural pattern** (a
  hand-rolled, fixed-size `VkDescriptorPool` for set-1 material params,
  `kMaxMaterialParamSets = 64`) -- NOT broken at the time of the original
  report (25 <= 64 for Sponza) and out of this P0's originally stated scope
  (the crash report named `sample_09_scene` only). **Closed in-round per
  coordinator instruction -- see section 7 below** rather than left as a
  follow-up: same defect class, found while this exact fix was fresh, so it
  closed now instead of going to the registry.
- **Packaging bundling is explicitly temporary** per the owner directive
  quoted in the task and in the commit message/code comments -- both the
  header-comment addendum and the staging block itself in
  `tools/package_samples.sh` are marked for removal once a real long-term
  Sponza distribution story is decided.
- **ctest coverage gap for `--scene <path>`** noted explicitly in section 3
  above (no headless ctest gate exercises the custom-import branch on any
  driver); the fix converts a driver-dependent crash into a clean, loud,
  portable failure if that branch's own sizing call is ever wrong again,
  but does not add ctest-level coverage of the sample's own call sites --
  doing so would need either extracting the sizing logic into a testable
  library seam or a new interactive/present-mode ctest entry, both judged
  out of scope for this P0.
- No board/issue/plan/spec/ledger files touched, no push performed, per
  task constraints.

## 7. In-round closure: `samples/08_gltf_viewer`'s identical pattern (`e86202c`)

Coordinator instruction: close the same-class defect in
`samples/08_gltf_viewer` now rather than defer it -- its own set-1
"material params" pool used the identical hand-rolled, fixed-size
`VkDescriptorPool` shape (`kMaxMaterialParamSets = 64`) as `09_scene`'s
pre-fix pool, built before the real material count was known. Not broken at
25 <= 64, but the same landmine one asset away, and `08_gltf_viewer`'s own
`--scene <path>` accepts ANY glTF asset (not gated to a curated Sponza
special-case like `09_scene`'s), so it is if anything MORE exposed.

**Fix (mirrors section 2 above exactly)**: `App::materialParamPool`
(`VkDescriptorPool`) -> `App::materialParamArena`
(`std::optional<rx::rhi::DescriptorArena>`); the `VkDescriptorSetLayout`
stays built unconditionally in `makeApp()`, but pool creation moved to a new
`createMaterialParamArena(App&, uint32_t materialCount)`. One structural
difference from `09_scene`: `08_gltf_viewer` has no separate grid/stress/
custom-scene modes -- `setupMaterials()` is the ONLY material-setup path,
invoked from inside `importGltfAsync()`'s completion callback (both
`runHeadless()`'s and `runPresent()`'s own async import). `createMaterialParamArena(*app,
static_cast<uint32_t>(result.materials.size()))` is now called at the top
of that SAME callback, before `setupMaterials()`, at both call sites. That
callback is confirmed main-thread-only by `registry.h`'s own D5 contract
("GPU uploads and every registry mutation are marshalled to the main
thread... this Registry's own main thread is whichever thread also calls
`scheduler.pumpMain()` each frame"), so building a `VkDescriptorPool` there
is safe. `setupMaterials()`'s own allocation now calls
`app.materialParamArena->allocate(app.materialParamSetLayout, 1)` instead of
a raw `vkAllocateDescriptorSets`. `destroyApp()` now just `.reset()`s the
`std::optional<DescriptorArena>`.

No new regression test added for this closure -- it reuses the exact same
`rx::rhi::DescriptorArena` mechanism section 3's Sponza-scale (8-vs-25) test
already pins; that test already proves the underlying arena-enforced
accounting discriminates deterministically on every driver, including
lavapipe, and both samples now route through the identical, already-covered
primitive.

### Verification

- **08's own headless ctest gates, isolated**:
  ```
  $ ctest --test-dir build/linux-native -R "sample_08" --output-on-failure
  1/2 Test #25: sample_08_gltf_viewer_headless ...........   Passed    1.54 sec
  2/2 Test #26: sample_08_gltf_viewer_quit_during_load ...   Passed    1.34 sec
  100% tests passed, 0 tests failed out of 2
  ```
  `sample_08_gltf_viewer_headless` includes the D17 pixel-tolerance
  reference-image gate -- passing confirms the pool-routing change produced
  byte-identical rendered output, as expected (same descriptor contents,
  different allocator underneath).

- **Real NVIDIA driver, labeled**: GeForce RTX 2080, driver 580.82.07,
  `VK_ICD_FILENAMES` pinned to `/usr/share/vulkan/icd.d/nvidia_icd.json`,
  real X11 session `:1`. `sample_08_gltf_viewer --present --validate`
  (default DamagedHelmet mode, no `--scene` override), sustained ~14s
  (`timeout 15`, SDL caught the terminating signal and shut down cleanly):
  ```
  [info] sample_08_gltf_viewer: material-params descriptor arena sized for 1 material(s)
  [info] sample_08_gltf_viewer: scene '.../DamagedHelmet.gltf' loaded -- 1 draw(s)
  ...
  [info] sample_08_gltf_viewer: window closed cleanly
  ```
  Zero unfiltered `[error]` lines (`grep -i "\[error\]" | grep -v "known
  false positive"` -> 0 matches); every match was the same pre-existing,
  already-documented "known false positive" sync-validation
  misclassification `context.cpp` already filters (unrelated to this
  change, present before and after).

- **Full serial ctest, once, lavapipe-labeled**: `VK_ICD_FILENAMES` pinned
  to `/usr/share/vulkan/icd.d/lvp_icd.json`, `xvfb-run`, real repo path:
  ```
  100% tests passed, 0 tests failed out of 29
  Total Test time (real) =  75.43 sec
  ```
  **29/29**, run after the 08 fix was built (includes both 08's own gates
  and every test from section 4b).

### Commit

`e86202c` fix(samples): 08_gltf_viewer material-params pool -- same-class
DescriptorArena fix as 09. Pathspec-scoped to
`samples/08_gltf_viewer/main.cpp` only, author = local git config (Yousef
Wadi), no AI attribution, not pushed.
