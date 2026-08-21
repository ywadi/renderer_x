# Review — Task 4 (#40): Camera exposure + physical-units API

Reviewer round. Commits under review: `8dab009` (implementation, parent
`26db15a`) + `35f9a55` (SDD report). Order of authority followed:
`rulings-2026-08-20.md` ("T4 (#40)") > plan >
`matrix-p5t04-camera-exposure.md` (rows as amended by the ruling) >
ticket #40. Independent of the implementer; every claim below is from
direct code reading, an independent tree-wide grep, a first-hand refetch
of Filament's pinned source, independently re-derived reference values,
reproduced build/test evidence on both drivers, and a self-applied
double-application mutation test — not from re-trusting the
task-04-report.md narrative.

## Verdict 1 — Spec compliance: **PASS**

Every matrix row (as ruled by T4: pre-exposure adopted, physical-units
shape per the matrix) is satisfied by the actual code:

- **Row 1/6 — `Camera` API shape.** `aperture`/`shutterSpeed`/`sensitivity`
  fields, `ev100()`/`exposure()` accessors, `setExposure(aperture,
  shutterSpeed, sensitivity)` (triple, clamped, clears override), and
  `setExposure(float ev100Override)` (direct override) all present in
  `src/rx_scene/include/rx_scene/camera.h`/`camera.cpp`, matching the
  matrix's row 1/row 6 proposed shape exactly, including the
  no-Filament-`Camera`-precedent honesty callout for the single-arg
  overload (see below).
- **Row 2 — exposure math, independently re-verified against Filament
  v1.75.0's actual `Exposure.cpp`.** Refetched
  `filament/src/Exposure.cpp` myself via `gh api
  repos/google/filament/contents/...?ref=v1.75.0` (tag SHA independently
  confirmed `0e58877c09afb1aacd09ff640f74d2adcd2a7e80`, matching RC1's
  pin) and diffed the three ported functions against it line-for-line:
  `ev100(N,t,S) = log2((N²/t)·100/S)`, the merged
  `exposure(N,t,S) = 1/(1.2·(N²/t)·100/S)`, and `exposure(ev100) =
  1/(1.2·2^ev100)` are byte-for-byte identical to the source (no
  off-by-log2, no missing/extra sqrt — there is no sqrt in this formula
  family). Also refetched `details/Camera.cpp`/`Camera.h` at the same tag:
  `MIN_APERTURE=0.5`/`MAX_APERTURE=64`/`MIN_SHUTTER_SPEED=1/25000`/
  `MAX_SHUTTER_SPEED=60`/`MIN_SENSITIVITY=10`/`MAX_SENSITIVITY=204800`
  and `mAperture=16.0f`/`mShutterSpeed=1/125.0f`/`mSensitivity=100.0f`
  match the port's clamp ranges and defaults exactly.
  **Independently recomputed all 5 rows** of `camera_test.cpp`'s
  device-free formula table via a from-scratch Python script (not the
  port's own code) straight from Filament's cited formulas — every
  expected `ev100`/`exposure` value matches the test's own hard-coded
  expectations to the digits given (daylight: 14.965784/2.604166667e-05;
  low light: -0.122256/0.9070294785; overcast: 11.965784/2.083333333e-04;
  indoor: 5.877744/1.417233560e-02; bright-sun-narrow-aperture:
  16.884648/6.887052342e-06). The `1.2` constant is cited with its
  saturation-based-sensitivity derivation in both `camera.h` and
  `camera.cpp`, matching this codebase's own constant-citation
  discipline.
- **Row 3 — pre-exposure, not full-float, adopted verbatim.**
  `RxMaterialGlobals`/`MaterialGlobalsPush` lost the `exposure` field
  entirely (`material.slang`, `draw_data.h`, `static_assert`-pinned at 8
  bytes, down from 12); `forward_entry.slang`'s
  `color.rgb *= exp2(gMaterialGlobals.exposure);` post-multiply is
  deleted, not dormant. Tree-wide grep for `exposure`/`exp2(` across
  `shaders/` turns up only comments referencing the deleted line — no
  live second application site anywhere in the shader tree, including
  both untouched `tonemap.frag.slang` copies (grep clean).
- **Row 4 — neutral-default byte-identical regression guard.**
  `exposureOverride` (`std::optional<float>`) defaults **engaged at
  1.0F**, distinct from what the photographic triple's own Filament
  defaults would resolve to (`~2.6e-5`) — correctly resolves the
  matrix's own flagged Conflicts-row-2 tension. Re-ran the D17 gates
  myself (see Empirical verification below): both samples reproduce
  their committed lavapipe references at exactly 0/65536 failing pixels
  with no `--exposure` flag.
- **Row 5 — exactly-once application; both producers scale BOTH terms.**
  Confirmed by direct read of current HEAD source (not just the diff):
  `samples/08_gltf_viewer/main.cpp`'s `updateDrawDataPerPassFields()`
  scales **both** `lightColor` and `ambientColor` by
  `app.exposureCamera.exposure()`; `samples/09_scene/main.cpp`'s
  `updateSceneFrame()` scales **both** `row.lightColor` and
  `row.ambientColor` by `app.flyCamera.camera.exposure()`. No missed
  term. `--exposure`'s CLI flag is preserved as a flag; its `0.0F`
  sentinel (documented at three sites) correctly bypasses
  `setExposure()` rather than feeding `ev100=0` through the formula,
  which is the only way the byte-identical default survives (verified:
  `exposure(0.0)≈0.833`, not `1.0`).
- **Row 6 — `setExposure(float ev100Override)` honestly documented as a
  RendererX extension.** Both `camera.h`'s field-level comment
  (`exposureOverride`) and its method-level comment (`setExposure(float
  ev100Override)`) explicitly state "no literal Filament Camera-level
  precedent — only Filament's free `Exposure::exposure(float ev100)`
  overload exists, never wired onto `FCamera`." Matches row 6's own
  finding; not misrepresented as a direct port anywhere in code or
  comments.

No matrix row was left unresolved, and the ruling's binding text (T4:
pre-exposure adopted, physical-units shape per the matrix) was followed
exactly — not the matrix's own tentative/tabled alternatives.

## Verdict 2 — Code quality: **Approved, with non-blocking documentation
findings**

The `Camera` exposure API itself, the shader/CPU pre-exposure wiring, and
the new tests are well-built: formulas verified byte-exact against the
pinned source, clamp ranges verified byte-exact, the neutral-default
mechanism is correctly reasoned and tested for the right invariant (not
just "equals 1.0" but "equals 1.0 *despite* the triple's own math not
giving 1.0"), and the double-application discriminator is a genuinely
new, well-targeted test pattern (independently re-proved below). No
correctness, build, or test-coverage defects found.

**Findings (both Low/Medium, non-blocking, comment/doc staleness only —
not driver-specific):**

1. **[Medium] `samples/README.md` still documents the OLD `--exposure`
   semantics.** Lines 736-739 ("`--exposure` (a pre-tonemap `2^exposure`
   multiplier applied inside the material forward pass...)") and lines
   786-787 ("**`--exposure <n>`** — pre-tonemap `2^n` multiplier (`0` —
   the default — is neutral, `2^0 == 1`)") were not updated by this
   task's diff (the file is absent from both commits' changed-file
   lists) and now describe a sign convention that is the **inverse** of
   the shipped behavior (higher now darkens, real EV100 units, not
   `2^n` brightening). A reader following this README would form the
   wrong mental model of the flag.
2. **[Medium] `MANUAL_VERIFICATION.md:448` states the inverted sign
   convention.** "`--exposure <n>` visibly brightens (positive) or
   darkens (negative) the rendered scene, pre-tonemap" is now backwards
   — under the shipped EV100 convention, a positive value darkens. This
   file is an operational checklist a human tester will actually follow
   later (its own "Last run" note confirms it has not been executed
   yet); as written, it would produce a result contradicting its own
   expected outcome. The report's own "Concerns for the coordinator"
   item (2) flags awareness of the semantics flip but neither commit
   updates this file or `samples/README.md` to match.
3. **[Low] `src/rx_material/material_system.cpp:628`** (a file this
   task did touch, just not this line) retains a stale comment: "All-
   scalar (uint/uint/float, 12 bytes) is expected to reflect with zero
   padding" — the struct is now uint/uint, 8 bytes, no float field. The
   actual check at line 635 compares against `sizeof(...)` dynamically,
   so behavior is correct; only the comment's field-shape description is
   wrong.
4. **[Low] `src/rx_material/tests/test_material_system.cpp:422-425`**
   (a file neither commit touches at all, despite referencing the
   deleted field) retains: "grew from one `uint` to
   `rx::material::MaterialGlobalsPush`'s three scalar fields (D26.1's
   `drawDataBufferIndex` + sample 08's `exposure`)." The struct now has
   two scalar fields and no `exposure` field; the assertion at line 427
   (`CHECK(layout.pushRanges[0].size == sizeof(rx::material::
   MaterialGlobalsPush))`) is dynamically correct and unaffected, so this
   is comment-only staleness that a build cannot catch (unlike the 9
   aggregate-init call sites the report's self-review says were caught
   by compile failures — this one wasn't, because it's prose, not code).

None of the four are correctness bugs; all are documentation/comment
drift introduced by the field removal and CLI semantics change that this
task's own grep discipline (cited in the report: "grep for
`MaterialGlobalsPush`/`RxMaterialGlobals` call sites") did not extend to
free-text comments/docs mentioning `exposure` outside the files it
already had open. Given this repo's stated no-deferred-fixes discipline,
these are real findings for this round, not registry material.

## Pre-exposure completeness — independently re-verified

Tree-wide grep for `exposure`/`exp2(` across `shaders/`, and for
`\.exposure\b`/`MaterialGlobalsPush{` (3-arg aggregate-init form) across
`src/`, `samples/`: the only remaining live hits are the new
`Camera::exposure()`/`exposureOverride`/`exposureMultiplier`/
`preExposure` names introduced by this task itself — zero leftover
push-constant-based or shader-side second application sites, zero stale
3-field aggregate initializers.

## Double-application-revert discriminator — re-proved myself, lavapipe

Reintroduced a synthetic "stray leftover second multiply" by temporarily
editing `test_standard_pbr_unlit.cpp`'s `makeRow` lambda in the "Camera
pre-exposure" `TEST_CASE` to apply `exposureMultiplier` **twice**
(`* exposureMultiplier * exposureMultiplier`), rebuilt
`rx_material_gpu_tests`, and ran it on lavapipe:

```
ERROR: CHECK( near8(brighterPixel.r, 85, 2) ) is NOT correct!
ERROR: CHECK( ... == doctest::Approx(brighterCamera.exposure() /
       neutralCamera.exposure()).epsilon(0.05) ) is NOT correct!
  values: CHECK( 2.78431 == Approx( 1.66667 ) )
```

Both the exact-pixel-value assertion and the linearity/ratio assertion
failed with exact, non-hand-waved values — `2.78431` is the squared
ratio `(5/3)² ≈ 2.778` (within the render's own quantization), not the
linear `5/3 ≈ 1.667` — precisely the failure signature the report's
self-review claims this test detects. Reverted the file
(`git checkout -- src/rx_material/tests/test_standard_pbr_unlit.cpp`),
confirmed `git diff --stat` empty for it, rebuilt, and reconfirmed the
real test passes clean (73/73 assertions) on lavapipe post-restore. The
discriminator is real, not asserted.

## Neutral-default D17 proof — re-run myself, lavapipe

Ran both samples' `--validate` mode directly (not just via ctest's
summary):

- `sample_08_gltf_viewer`: `D17 loading_state gate: failingPixels=0/65536
  (0.0000%) pass=true`; `D17 loaded_scene gate: failingPixels=0/65536
  (0.0000%) pass=true`.
- `sample_09_scene`: `D17 grid_scene gate: failingPixels=0/65536
  (0.0000%) pass=true`.

Both exactly `0/65536` with no `--exposure` flag, matching the report's
claim and confirming no reference PNG needed regeneration for this task
(`git show --stat` on both commits confirms no `*.png` touched).

## `--exposure` CLI semantics change — verified honest in code, NOT in
docs (see findings 1/2 above)

`Args::exposure`'s own field comment, `App::exposureCamera`'s own
comment, and the file's top-of-file EXPOSURE section
(`samples/08_gltf_viewer/main.cpp`) all accurately state: real EV100
units now, higher darkens (inverse of the old `2^x` convention), `0.0F`
is a sentinel not a formula input. This sample has no separate
`--help`/usage banner beyond these code comments — the only genuinely
stale, user-facing prose describing the flag is `samples/README.md` and
`MANUAL_VERIFICATION.md` (findings 1/2).

## Empirical verification performed (driver-labeled)

- Full serial `ctest -j1`, **lavapipe** (`lvp_icd.json`, `xvfb-run`,
  NICEd, foreground): **29/29 passed**, 80.34s.
- `rx_material_gpu_tests` (the one GPU test binary this task's diff
  touches), **real NVIDIA GeForce RTX 2080** (default ICD, NICEd,
  foreground): **57/57 test cases, 2549/2549 assertions passed**, zero
  unfiltered `[vulkan validation]` lines (every warning matches this
  codebase's own documented false-positive filter set).
- D17 gates (`sample_08_gltf_viewer`, `sample_09_scene`), default
  args, **lavapipe**: both `0/65536` — see above.
- D17 spot-check, `sample_08_gltf_viewer`, **real NVIDIA RTX 2080**
  (informational-only path): `loading_state 0/65536`, `loaded_scene
  425/65536 (0.6485%) pass=false [non-lavapipe driver -- informational
  only, not enforced]`, overall `headless gate PASSED` — matches the
  report's cited number exactly; confirmed this is the same
  pre-existing driver-divergence shape T3's own review already
  documented, unrelated to this task.
- Double-application-revert re-proof (mutation + exact-value failures +
  byte-identical restore) — see above, lavapipe.
- Formula-table + clamp-range re-derivation against Filament v1.75.0's
  actual pinned source (`gh api`, tag SHA `0e58877c...` independently
  confirmed) — see Verdict 1, row 2.
- Commit hygiene: exactly 2 commits (`8dab009` implementation,
  `35f9a55` report), parent `26db15a`; `git show --stat` on `8dab009`
  lists exactly the 12 files the report's "What shipped" section names
  (`.superpowers/sdd/.../progress.md` correctly excluded, confirmed
  concurrently modified by another agent in this shared tree and not
  present in either commit's diff); author/committer `Yousef Wadi
  <ywadi85@gmail.com>` on both commits, matching local `git config`;
  both commit messages and the report grepped for
  `claude|anthropic|co-authored|ai-generated|generated by` — zero hits;
  `git rev-list --left-right --count origin/main...HEAD` → `0  2`
  (branch is exactly 2 commits ahead of `origin/main`, 0 behind, nothing
  pushed).

**Not independently re-run this round** (outside the stated empirical
minimum, and this task touches no Windows-relevant code path — device-
free tests, shader edits, and samples' own `main.cpp` are all
cross-platform-generic): the windows-cross-zig 38/38 build + 13/13 ctest
claim. Flagged as not independently verified rather than silently
assumed true.

## Not independently verifiable this round

- Whether `filament/src/details/Camera.cpp`'s `Exposure.cpp` formulas
  have ever changed at any point in Filament's history before v1.75.0 —
  out of scope; this review (like the matrix and report) verifies only
  against the pinned tag, which is what RC1 requires.
- Human-observed, on-display confirmation that `--exposure` "visibly"
  darkens/brightens as documented in code comments — `MANUAL_VERIFICATION.md`
  itself records this as not yet performed on real display hardware
  (unrelated to this task; pre-existing status), and finding 2 above
  means its own text would currently describe the wrong direction even
  if it were run today.

## Restoration

The only working-tree edit made during this review (`test_standard_pbr_
unlit.cpp`'s `makeRow` lambda, temporarily squaring the exposure
multiplier for the double-application re-proof) was reverted via `git
checkout -- <path>` and confirmed via `git diff --stat` showing zero
changes for that file, then `rx_material_gpu_tests` was rebuilt and
reconfirmed green (73/73 assertions) on lavapipe post-restore. `git
status --porcelain=v2` at review end shows only the pre-existing
`.superpowers/sdd/2026-08-20-phase5-techniques/progress.md`
modification, byte-identical to its state at review start — left alone
per instruction.
