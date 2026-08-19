# Independent review: helmet sampler-wrap P0 fix round

Reviewer scope: commits `715681b`, `2f584c4`, `787f978` (base `f67de3d`),
against `helmet-sampler-fix-brief.md`'s six mandatory items. All findings
below are empirical (built and run locally on this machine), not taken on
the implementer's word, except where explicitly marked "not independently
verified."

## Verdicts

**Spec compliance: PASS.** All six brief items are met; the two binding
constraints called out for special attention (per-slot wiring on both
StandardPbr and Unlit; REPEAT-not-clamp default) are both satisfied and
independently reproduced.

**Code quality: Approved**, with one Important and three Minor findings
(none blocking).

## What was independently verified (empirical, this session)

1. **Regression-test revert/restore — done twice, exceeding the brief's own
   bar.**
   - In-tree revert (matching the implementer's fallback methodology): with
     `getOrCreateSampler()`'s `key.wrapS`/`key.wrapT` re-hardcoded to
     `CLAMP_TO_EDGE`, `rx_asset_tests --test-case="Sampler-wrap regression*"`
     failed **1/1 case, 21/26 assertions, 5 failed** — the exact 5
     discriminating assertions (all four quadrants reading BLUE instead of
     RED, plus the `CHECK_FALSE` also failing), byte-for-byte matching the
     report's own numbers. Restored via `git checkout --`, confirmed empty
     `git diff --stat`, re-ran: **1/1, 26/26 SUCCESS**.
   - **The literal scratch-worktree revert the brief actually mandated was
     also completed by this review**, closing the implementer's own
     documented gap (see Concern 1 below): `git worktree add` at `787f978`
     to a scratch path, symlinked `toolchain/` in (the same convention
     already visible in two other live agent worktrees under
     `.claude/worktrees/` in this same repo), configured, and built
     `rx_asset_tests` clean on the first attempt — no ubsan/mikktspace
     linking failure reproduced. Ran the same revert/rebuild/re-run sequence
     entirely inside that worktree: fixed → 26/26 SUCCESS; reverted → 5/26
     failed (identical assertion IDs); worktree then removed
     (`git worktree remove --force`), main tree untouched throughout.
2. **rx_asset_tests**: 33/33 cases, 564/564 assertions, SUCCESS (matches
   report exactly).
3. **rx_material_tests**: 14/14, 75/75, SUCCESS (matches).
4. **rx_material_gpu_tests**: 49/49, 2265/2265, SUCCESS (matches).
5. **rx_asset_gltf_tests**: 48/48, 292/292, SUCCESS (matches).
6. **rx_asset_gltf_gpu_tests**: 57/57 passed; assertion total
   (8,590,761) differs from every count in the report's own table — this is
   itself consistent with the documented pre-existing wall-clock/
   iteration-budget flake (assertion count varies run to run; the suite
   still passed clean here). See Concern 3.
7. **Full serial `ctest --output-on-failure -j1`**: 22/22 passed (228.52s
   this run), including `sample_08_gltf_viewer_headless` and
   `sample_08_gltf_viewer_quit_during_load`.
8. **D17 gate, lavapipe-forced**: fresh incremental build of
   `sample_08_gltf_viewer`, run with `VK_ICD_FILENAMES` pointed at the real
   lavapipe ICD: `loading_state` and `loaded_scene` both
   `failingPixels=0/65536 (0.0000%) pass=true`. A second run on this
   machine's default (non-lavapipe) driver correctly reported
   `[non-lavapipe driver -- informational only, not enforced]` and did not
   fail the gate — expected, documented behavior, not a regression.
9. **Zero unfiltered Vulkan validation errors** across every run above,
   including a `--validate` run's full teardown log: every hit is
   `SPIR-V SourceLanguage=Slang` or the separate-sampler
   `SYNC-HAZARD-READ_AFTER_WRITE` misclassification, both pre-existing and
   tagged "known false positive" in-code.
10. **Shader-deploy DEPENDS sweep, full manual audit** (not just the diff):
    grepped every `samples/*/CMakeLists.txt` for `add_custom_command`. Every
    remaining bare `POST_BUILD` (no `DEPENDS`) is legitimately out of scope:
    `03_bindless_mesh` only deploys `texture.png` (a PNG asset, not a
    shader; its shader is compiled in-process from an embedded string, no
    file to go stale); `04_streaming` has no deploy step of any kind (same
    in-process compilation, no external assets at all). Every sample that
    deploys a `.slang` file now uses the `OUTPUT`+stamp+`add_custom_target`
    pattern. Confirmed complete, not just the two steps the brief named
    explicitly.
11. **Per-slot sampler wiring, all slots, both material types**: confirmed
    in the diff and by reading current source —
    `StandardPbrParams` carries `baseColorSampler`/`metallicRoughnessSampler`/
    `normalSampler`/`occlusionSampler`/`emissiveSampler`; `UnlitParams`
    carries `baseColorSampler`; every one of the 6 `rx_sampleTexture()` call
    sites in `standard_pbr.slang`/`unlit.slang` uses the new 3-argument
    overload. `setupMaterials()` (`samples/08_gltf_viewer/main.cpp`) wires
    `resolveSamplerIndex()` for every slot in both branches (isUnlit and
    StandardPbr).
12. **Absent-sampler default is REPEAT, not clamp**, in both places that
    matter: `samples/08_gltf_viewer/main.cpp`'s own `app.defaultSampler`
    (`VK_SAMPLER_ADDRESS_MODE_REPEAT`) and
    `MaterialSystem::create()`'s `defaultSamplerInfo` (new
    `defaultSamplerAddressMode` parameter, default
    `VK_SAMPLER_ADDRESS_MODE_REPEAT`). `SamplerDesc`'s own default-
    constructed values (`mesh_asset.h:128-129`) are `wrapS = wrapT = 10497`
    (glTF `REPEAT`), confirmed by direct read — corroborates the "default-
    constructed IS the spec default" claim used throughout the fix's
    comments.
13. **Task 4 seam-bleed test not weakened**: `checkQuadrantPixels()`
    (`test_api_factory.cpp:1126-1131`) is byte-identical to its pre-fix-round
    form (`std::memcmp(...) == 0` on all four corners, diffed directly
    against the pre-fix-round blob at `22abfed`) — only the
    `MaterialSystem::create()` call site changed, to pass
    `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE` explicitly as the new 4th
    argument. Same for the hot-reload sibling test.
14. **Commit hygiene**: all three commits authored/committed as
    `Yousef Wadi <ywadi85@gmail.com>` (local git config), zero AI-attribution
    strings in any commit message, `origin/main` (`ed5239a`) unreached —
    nothing pushed. Per-commit file lists are cleanly pathspec-scoped and
    match the report's own §10.7 "files touched" list exactly: `715681b`
    is 20 production/test files, `2f584c4` is the single PNG, `787f978` is
    the report addendum + `helmet-after.png`. No board/plan/spec/ledger file
    in any of the three.
15. **`helmet-after.png` visual sanity**: read directly. Shows a teal visor,
    silver/white panels, dark-green dome with cyan vent-light detail, gold
    mouth-guard accent — matching the independent `helmet-reference-noibl.png`
    Blender no-IBL render essentially exactly. `helmet-before.png` (the bug)
    is a near-uniform dark mottled dome with no legible detail, consistent
    with "the whole mesh sampling one bottom-edge texel row." Confirmed
    `helmet-after.png` is 512x512 and `helmet-before.png` is 256x256 via
    direct pixel-dimension read (PIL), corroborating the documented
    upscale claim (see Concern 2).
16. **`bindInstance()`/D26.1 push-constant wiring not broken by the
    refactor**: `push.defaultSamplerIndex = impl.defaultSamplerHandle.index()`
    (material_system.cpp:1698) and `push.defaultSamplerIndex =
    app.defaultSamplerHandle.index()` (main.cpp:1418) both still populate
    the two-argument overload's fallback correctly; this field is provably
    unused for DamagedHelmet's own five slots (all resolve through the new
    per-slot fields) but remains coherent as the belt-and-suspenders path.

## Not independently verified

- `windows-cross-zig` build/ctest (Wine). Time-boxed out of this review's
  scope; taken on the report's word (154/164 targets, 22/22 ctest under
  Wine). This is the one substantive claim in the report I did not
  reproduce myself.
- A from-scratch `rm -rf build/linux-native` full clean rebuild was not
  repeated by this review (the report's own §10.6 did this once already);
  this review instead did an incremental build plus a full serial `ctest`
  run, which exercised the same D17 gate and the same test binaries and
  came back with identical pass/fail shape and near-identical numbers.

## Concern adjudications

**1. Worktree fallback — VERDICT: gap closed by this review; Minor process
finding stands.** The implementer's own diagnosis (a `~/.cache/zig`
content-addressed-cache interaction causing 11 unresolved
`__ubsan_handle_*` symbols specifically from a worktree-relative
`mikktspace.c` build) could not be reproduced here: a scratch worktree,
once `toolchain/` was symlinked in (the exact convention already
established by two other live agent worktrees under `.claude/worktrees/`
in this same repo), configured and built clean on the first attempt,
including `mikktspace.c`/`mikktspace_bridge.cpp`/`librx_asset.a` linking.
The brief's "scratch-worktree revert evidence mandatory" requirement is now
satisfied — by this review, reproducing the implementer's exact claimed
in-tree numbers inside a genuine worktree. This does not retroactively
invalidate the implementer's fix (the underlying revert-test result was
correct either way, confirmed three independent ways now: their in-tree
run, my in-tree run, my worktree run, all identical). But the fallback
itself was avoidable: the toolchain-symlink convention was discoverable
in-session by inspecting sibling worktrees in the same repo before
invoking the item-5 in-tree-revert precedent. Minor finding, not blocking.

**2. `helmet-after.png` as a 512px upscale of a native 256px capture —
VERDICT: acceptable, transparently disclosed.** Confirmed by direct pixel
read: 512x512 vs. `helmet-before.png`'s 256x256. The report states plainly
that the sample's headless capture has no resolution flag, that there is no
native 512px render, and that the upscale is Lanczos/presentation-only —
this is disclosed in the same sentence as the claim, not buried or implied
otherwise. The D17 gate itself (the thing that actually matters for CI) is
enforced at the native 256x256 resolution and passes 0/65536 both frames.
Not a defect.

**3. Pre-existing `gltf_gpu` wall-clock flake, left un-chased — VERDICT:
acceptable to leave un-chased.** Reproduced the same symptom shape myself:
57/57 passed on this run, but the total assertion count (8,590,761) matches
none of the report's own prior runs' counts either — consistent with a
genuinely load/timing-sensitive count, not a fixed number that happened to
drift. The report correctly identifies this as pre-existing (documented
before this fix round even started, in the original investigation's own
§6), unrelated to any file this fix round touches (no sampler/texture-cache
file is in that binary's link graph per the report's own claim, which is
plausible given the binary name split), and flags it for the coordinator
rather than silently absorbing scope creep. Consistent with this project's
"no deferred fixes... only feature phase-fits go to the registry" policy
only insofar as this was never this task's fix surface to begin with — it
predates the P0 entirely. Correctly out of scope.

## Findings

**Important:**
- The new regression test (`texture_cache_test.cpp`, "Sampler-wrap
  regression") exercises `Registry::importGltf()` →
  `TextureCache::getOrCreateSamplerBindlessIndex()` → a raw bindless
  test-shader `Sample()` call — genuinely the full glTF-import-to-GPU-sample
  path, and it is the exact call `resolveSamplerIndex()` makes in
  production. It does **not**, however, render through the actual shipped
  `standard_pbr.slang`/`unlit.slang` material shaders or through
  `StandardPbrParams`/`UnlitParams` — so a future regression specifically in
  how `setupMaterials()` maps a resolved sampler index into the *correct*
  per-slot param field (e.g., an accidental `metallicRoughnessSampler`/
  `normalSampler` swap) would not be caught by this unit test. The only
  test that exercises the real production shader end-to-end is the D17
  `sample_08_gltf_viewer_headless` pixel gate, which is coarse (whole-image
  diff, not slot-attributable) and depends on DamagedHelmet's specific
  asset content rather than a purpose-built discriminating fixture. This is
  a real, if narrow, coverage gap between "the sampler-creation/resolution
  layer is correct" (well covered) and "the per-slot param wiring in
  `setupMaterials()` puts the right sampler on the right slot" (covered
  only by DamagedHelmet's own five slots happening to share one sampler
  object, which cannot discriminate a slot-crossed sampler assignment the
  way the pre-existing "Slot-swap discrimination" texture test discriminates
  crossed texture indices). Not a blocking defect — the shipped code is
  correct by direct inspection (`main.cpp:1054-1068`, five discrete
  `resolveSamplerIndex()` calls each keyed to its own slot's `TextureRef`)
  — but a natural follow-up would be a StandardPbr-level (not
  TextureCache-level) sampler slot-swap test, mirroring the existing
  texture slot-swap test's own shape.

**Minor:**
- Worktree-fallback process gap (Concern 1 above) — the established
  toolchain-symlink convention for scratch worktrees in this repo was not
  checked before falling back to an in-tree revert.
- `renderCustomQuadAndReadbackQuadrants()` (new test helper,
  `texture_cache_test.cpp`) is explicitly documented as a "near-identical
  duplicate" of the pre-existing `renderAndReadbackQuadrants()` rather than
  a parameterized extension of it — a deliberate, justified choice (keeps
  every existing caller at zero risk) but it does mean the file now carries
  two ~90-line GPU-pipeline-setup bodies that will drift independently if
  either is touched later without touching the other. Acceptable given the
  stated rationale; flagging for future consolidation only.
- `gltf_gpu` flake (Concern 3) remains formally untriaged after two fix
  rounds' worth of "flagging for the coordinator" — still correctly out of
  this task's scope, but the registry should pick this up explicitly if it
  has not already, since "flagged in a report" is not the same as "tracked."

No Critical findings.

---

## ROUND 2: scoped re-review (commits `026186f`, `64a0b84`)

Scope: verify ONLY the two round-1 findings above were closed. No new broad
review performed.

### Verdict: ALL ADDRESSED

**Finding 1 [Important] — StandardPbr-level per-slot sampler coverage gap
— ADDRESSED.**
New GPU `TEST_CASE` ("StandardPBR per-slot sampler wiring: ...",
`test_standard_pbr_unlit.cpp`) renders through the real
`MaterialSystem`/`standard_pbr.slang`/`StandardPbrParams` path, exactly
closing the gap as scoped: one 1x2 striped texture bound to both
`baseColorTexture` and `emissiveTexture`, sampled at the identical
out-of-`[0,1]` UV through two different real samplers
(`baseColorSampler=REPEAT`, `emissiveSampler=CLAMP_TO_EDGE`). Verified the
isolation logic directly against `shaders/material/standard_pbr.slang`:
`color = directLight + ambient + emissive`, with `directLight` zeroed
unconditionally by `lightColor=(0,0,0)` and `ambient`/`emissive` each
independently zeroed via their own `*Factor` in the two draws — so each
draw's pixel is provably a direct, unblended read of exactly one slot's own
sampled texel; a slot-crossed field assignment has no other way to produce
the predicted inversion. Re-ran `rx_material_gpu_tests` myself: **50/50,
2379/2379, SUCCESS**, zero unfiltered validation errors (both full-suite
and isolated `--test-case="StandardPBR per-slot sampler wiring*"` →
**1/1, 114/114**). Did not take the revert evidence on the report's word
alone — the in-code comment's claim ("failed all 6 assertions below") did
not match the report's own numeric claim ("4 FAILED"), so re-proved it
directly: swapped the two `makeBlob()` sampler arguments in place, rebuilt,
re-ran → **1/1 FAILED, 110/114 passed, 4 FAILED**, byte-for-byte matching
the report's addendum (`baseColorPixel` read BLUE not RED: `r>200`/`b<50`
failed, `g<50` still passed since green=0 in both colors; `emissivePixel`
read RED not BLUE: `b>200`/`r<50` failed, `g<50` still passed) — confirming
the report's "4 FAILED" is the accurate number and the in-code comment's
"all 6" is a minor, non-substantive inaccuracy (does not affect the test's
actual discriminating power). Restored via `git checkout --`, confirmed
empty `git diff --stat`, re-ran → **1/1, 114/114 SUCCESS** again. The
`BindlessTable::Capacities::samplers` bump (2→4 in this file's
`makeFixture()`) is real and necessary (3 live samplers now registered
against a capacity of 2 would exhaust it) and purely permissive for every
other `TEST_CASE` in the file.

**Finding 2 [Minor] — `renderCustomQuadAndReadbackQuadrants()` duplication
— ADDRESSED.**
Confirmed via diff read: the ~90-line duplicate function is deleted;
`renderAndReadbackQuadrants()` gained one defaulted trailing parameter
(`vertices = kQuadVertices`), and the round-1 regression test's two call
sites now pass their custom vertex arrays as that argument instead of
calling the now-removed duplicate. Every other pre-existing call site is
untouched (no argument added), matching the "behavior-preserving by
construction" claim. Re-ran `rx_asset_tests` myself: **33/33, 564/564,
SUCCESS** — unchanged from round 1's own numbers, confirming the dedup
introduced no behavior change.

**Scope check**: the diff touches exactly `helmet-texture-fix-report.md`
(docs), `src/rx_asset/tests/texture_cache_test.cpp`, and
`src/rx_material/tests/test_standard_pbr_unlit.cpp` — nothing outside the
two findings' own files. No production code
(`material_system.cpp`/`texture_cache.cpp`/shaders/`main.cpp`) was touched,
consistent with both findings being test-only closures. Commits
`026186f`/`64a0b84` both authored `Yousef Wadi <ywadi85@gmail.com>`, zero
AI-attribution strings, cleanly pathspec-scoped to exactly the files each
commit's own description claims, `origin/main` still unreached (nothing
pushed).

No new findings raised by this scoped re-review.
