# Independent review — Draco round (issue #31)

Reviewed: 5 commits on `main`, none pushed (`a3858a4`, `2801c99`, `f1fe6bc`,
`33be185`, `740fd96`), base `11dbc23`. Reviewer did not write this code.
Verification performed empirically on real hardware (NVIDIA GeForce RTX
2080, driver 580.82.07 — same box the implementer used) and lavapipe, plus
a windows-cross-zig / Wine spot-check. All temporary edits made during this
review were reverted byte-identically; the pre-existing uncommitted
modification to `progress.md` was left untouched throughout.

## Verdicts

**Spec compliance (issue #31 acceptance criteria): PASS.** All five
criteria are met by real, independently-reproduced evidence (detail below).
Two non-blocking findings surfaced during verification (see Findings).

**Code quality: Approved, with 2 medium findings.** No blocking defects.
Build is clean (zero warnings, both presets), commit hygiene is correct,
provenance/licensing claims verified accurate against upstream, no
duplicated gate logic. The two findings are a report-accuracy defect and an
undisclosed-scope test-coverage gap discovered by extending the
implementer's own revert-discrimination work — neither invalidates the
round's evidence, both are worth closing in a follow-up.

## "Already existed" claim — CONFIRMED

`git blame`/`git log` on `decodeDracoPrimitive()` (`src/rx_asset/import_gltf.cpp`
~508-614): the entire function was added in a single commit, `337d368`
("feat(rx_asset): glTF 2.0 import core (fastgltf + MikkTSpace + meshoptimizer
+ Draco)"), dated 2026-08-18 14:56:43 — the same day as the vendoring commit
`a8bf4f9` (14:08:21). Issue #31 itself was filed 2026-08-20 per the report.
None of this round's 5 commits (all dated 2026-08-20 14:03-14:16) touch
`import_gltf.cpp` at all (`git log --format=%H a3858a4^..740fd96 --
src/rx_asset/import_gltf.cpp` is empty). The round is exactly what it
claims to be: fixtures/tests/tooling on top of a pre-existing, untouched
decode path.

## Acceptance criteria, one by one

1. **Committed Draco fixture + uncompressed twin, full import→render,
   exact-counter/pixel gates.** PASS — reproduced directly. Counter test
   (`draco_compression_test.cpp`) passes on both drivers (72/72 assertions
   across all 4 draco/BoomBox cases). Pixel gate reproduced independently:
   rendered `cube_draco.gltf` and `cube_draco_reference.gltf` through
   unmodified `sample_08_gltf_viewer --write-references`, compared with
   `compare_rgba8_png` — **0/65536 failing pixels on both real NVIDIA and
   lavapipe**, matching the report exactly.

   **Load-bearing verification (extends the report):** the report only
   showed the counter test failing under an injected attribute-id-swap bug;
   it never checked whether the pixel gate would also fail. I injected the
   identical bug (swapped `findAttr("POSITION")`/`findAttr("NORMAL")` in
   `decodeDracoPrimitive()`), rebuilt, and confirmed **both** gates
   discriminate: the counter test fails (2/3 cases, 3/49 assertions —
   identical to the report's own numbers), and the pixel gate fails with
   **7500/65536 (11.44%) failing pixels**, well over the 0.5% budget. The
   twin comparison is genuinely load-bearing on both axes, not just the
   counter side. Edit reverted; `git diff` confirmed byte-identical
   restoration before moving on.

2. **Real-world Draco asset (Sketchfab-class), headless, real NVIDIA +
   lavapipe.** PASS. BoomBox (glTF-Draco variant) fetched and gate-tested;
   confirmed against upstream: `extensionsRequired=["KHR_draco_mesh_compression"]`,
   1 mesh/1 material, `accessors[0].count=18108` (matches the test's
   assertion exactly), all four PBR texture slots + TANGENT present. License
   verified directly against `glTF-Sample-Assets/Models/BoomBox/LICENSE.md`
   and `metadata.json` on GitHub: CC0-1.0, artist "Microsoft" — matches
   `ASSET-NOTES.md` and `fetch_assets.sh`'s claims exactly. All 6 pinned
   SHA-256 checksums in `tools/fetch_assets.sh` verified byte-for-byte
   against the actually-fetched files in `assets/fetched/BoomBox/glTF-Draco/`.
   Test passes 23 assertions on real NVIDIA, lavapipe, and under Wine
   (windows-cross-zig) — all three reproduced. "Sketchfab-class" read as
   descriptive rather than a literal sourcing requirement is reasonable;
   Khronos's own canonical Draco compatibility fixture is a defensible,
   arguably stronger choice.

3. **Non-Draco imports byte-identical.** PASS. Full serial `ctest`,
   both drivers: **29/29** (real NVIDIA: 145.92s; lavapipe: 80.39s). No
   pre-existing test file or fixture was touched by this round's diff.

4. **Decode failure → clear actionable error, never a crash.** PASS,
   reproduced directly. Running the corrupt fixture logs exactly:
   `[error] rx_asset: Draco decode failed: Failed to decode geometry data.`
   File-level import completes (`result.ok()==true`), mesh registers with
   zero submeshes and invalid bounds, no crash — matches the report
   verbatim.

5. **Revert-discrimination evidence for the decode path's load-bearing
   test.** PASS on the letter of the criterion (Attempt 2 — the
   `findAttr()` name-swap — discriminates cleanly, reproduced above and
   extended to the pixel gate). See the honest-negative ruling below for a
   qualification.

6. **Registry import notes updated.** PASS, as a pre-existing fact:
   `import_gltf.cpp`'s extension-disposition map already carries
   `{"KHR_draco_mesh_compression", "consumed(decoded)"}` and appears live
   in every test run's log line. Reading "registry" as this in-code tag
   rather than a markdown ledger is a reasonable, if convenient,
   interpretation — no gate/matrix/ledger document was in scope or touched.

## 08-vs-09 adjudication

**The report's stated justification, as written, is factually wrong.**
`grep -n "scene" samples/09_scene/main.cpp` finds a real `--scene <path>`
flag (`main.cpp:206-207`), referenced in ~15 other places in that file for
Sponza/present-mode/custom imports. The report's claim — "`samples/09_scene`
has no `--scene` flag at all (grep-confirmed)" — does not hold up under the
same grep the report claims to have run. This is a direct, checkable
factual error inside a document whose own §7 self-review states "every
verification command in this report was actually run this round" — a
report-accuracy defect regardless of what the author intended.

**The engineering choice underneath it is correct once restated
correctly.** I traced `runHeadless()` (`samples/09_scene/main.cpp:2550`
onward) — the function that actually drives sample 09's exact-counter/pixel
gate — and confirmed it unconditionally calls `resolveHelmetScenePath()` in
the non-stress path (line 2599), **never reading `args.scenePath` at all**.
The `--scene` flag is wired only into the `--present` (interactive) and
`--stress` code paths, not into the headless gate machinery. So the real,
defensible reason sample 09 wasn't retrofit-worthy is: *its headless
pixel-gate machinery is hardcoded to a DamagedHelmet-grid composition and
was never built to accept an arbitrary second scene*, not "it has no
`--scene` flag." Sample 08's `--scene <path>` + `--write-references` is
confirmed generic (no hardcoded scene path anywhere in its own flag
handling), making it the lower-risk, correctly-chosen vehicle. **Ruling:
the engineering decision is sound; the report's justification for it is
inaccurate as written and should be corrected.**

## Honest-negative ruling (`mapped_index()` bypass)

The implementer's Attempt 1 disclosure (reverting `mapped_index()` to a
naive `AttributeValueIndex(pi.value())` cast, finding it doesn't
discriminate for `cube_draco.gltf`) is accurate and was reported honestly
— I reproduced it exactly (49/49 assertions still pass).

**However, independent follow-up shows the report's framing — "a
coincidental no-op here… not evidence the test itself is weak" — understates
a real gap.** I instrumented both fixtures to report Draco's internal
attribute-value counts:

- `cube_draco.gltf`: `numPoints=6`, `posAttr.size()=6` — the explicit
  mapping happens to be sequential 1:1, so the naive cast is a genuine
  coincidental no-op, exactly as claimed.
- `BoomBox.gltf`: `numPoints=3575`, `posAttr.size()=3237` — a real
  many-to-one mapping (fewer unique attribute values than points, typical
  of a mesh with UV/normal seams). I confirmed in Draco's own vendored
  header (`GeometryAttribute::GetValue`/`GetAddress`,
  `.deps-cache/draco-*/include/draco/attributes/geometry_attribute.h:118-139`)
  that these functions do **zero bounds-checking** — `byte_pos =
  byte_offset_ + byte_stride_ * att_index.value()` is used directly against
  the underlying buffer. Reapplying the same naive-cast bypass and running
  **all four** draco/BoomBox test cases together still produces **72/72
  assertions passing, 0 failures** — including the BoomBox integration
  test, which reads out-of-bounds into Draco's internal attribute buffer
  (undefined behavior) without the test suite noticing, because none of
  the BoomBox test's assertions check exact decoded attribute values —
  only index count, `bounds.isValid()`, material fields, and skin absence.

**Ruling: this is a genuine, if narrow, coverage gap in the committed test
suite, not merely an artifact of one trivial fixture.** It reaches the
round's own flagship "real-world asset" acceptance-criterion fixture, and
there the failure mode is worse (UB) than the "harmless no-op" framing
suggests. It does not block this round — criterion 5 is satisfied on its
own terms by Attempt 2, and the disclosure itself was made in good faith —
but it is a real finding worth a follow-up (e.g., a value-level assertion
on a known BoomBox vertex, or extending the render pixel-gate to BoomBox).

## Tooling scope (`compare_rgba8_png`, `gen_gltf_compression_fixtures`)

Both reasonably scoped, no duplication found. `compare_rgba8_png` links
`sample_common` directly and calls its existing `loadRgba8Png()`/
`compareToReference()` (D17 algorithm) — confirmed present in
`samples/common/reference_gate.h` — rather than reimplementing pixel
comparison. `gen_gltf_compression_fixtures`'s new functions
(`genDracoReferenceFixture`, the corrupt-fixture block) reuse the existing
`writeFile`/`gltfHeader` helpers and a new `kSharedMaterialJson` constant
shared across both new fixtures, avoiding duplication within the tool
itself. The "gate ruling #15" ("no second regeneration mechanism") citation
checks out as a real, repeatedly-invoked prior ruling across this repo's
git history (found in `docs/superpowers/specs/`, sample 09 commits, etc.),
not a fabricated reference.

## Empirical verification performed this round

- Full serial `ctest`, real NVIDIA (RTX 2080, driver 580.82.07, default
  ICD): **29/29 passed**, 145.92s.
- Full serial `ctest`, forced lavapipe (`VK_ICD_FILENAMES` + `xvfb-run`):
  **29/29 passed**, 80.39s.
- Draco/BoomBox tests run directly on both drivers: 4/4 cases, 72/72
  assertions, both drivers, plus under Wine (windows-cross-zig): 4/4, 72/72.
- Pixel-equivalence gate (`cube_draco.gltf` vs. `cube_draco_reference.gltf`
  via `sample_08_gltf_viewer --write-references` + `compare_rgba8_png`) on
  both real NVIDIA and lavapipe: 0/65536 failing pixels both times.
- Zero warnings confirmed on all touched files (`import_gltf.cpp`,
  `draco_compression_test.cpp`, `gen_gltf_compression_fixtures/main.cpp`,
  `compare_rgba8_png/main.cpp`), both presets, via forced rebuild + grep.
- `windows-cross-zig` full build of all touched targets: clean, zero
  errors, zero warnings.
- Wine spot-check: `rx_asset_gltf_gpu_tests` (contains both the draco tests
  and `async_import_test.cpp`) via `ctest -R`. **Run 1: hit the same
  pre-existing wall-clock flake the report disclosed**
  (`async_import_test.cpp`'s calibration gate, `REQUIRE(maxPumpDuration <
  kCiStallDetector)`, observed `6990µs < 6464µs` failing) — confirmed this
  is in a file untouched by this round's diff. **Run 2: passed cleanly**,
  45.65s, 0 failures. Matches the report's own documented behavior exactly;
  not chased further per instructions.
- Provenance/licensing independently verified against live upstream
  (`raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets`): LICENSE.md
  and metadata.json confirm CC0-1.0 / Microsoft / 2017, matching
  `ASSET-NOTES.md` exactly. All 6 SHA-256 checksums verified byte-exact.

## Commit hygiene

- 5 commits, all dated 2026-08-20, author `Yousef Wadi
  <ywadi85@gmail.com>` — matches local git config exactly, no override.
- `git log --format=%B a3858a4^..740fd96 | grep -iE
  "claude|anthropic|co-authored|generated with"` — empty on all 5 commits.
- Pathspec scope: each commit's `git show --stat` file list matches the
  report's own §6 claims exactly (fixtures commit touches only fixture
  files + the generator tool; test commit touches only the new test file +
  its CMakeLists.txt registration; etc.) — no stray files, no `git add -A`
  footprint.
- Nothing pushed: `origin/main` is at `11dbc23` (the review package's own
  base commit); local `main` is exactly these 5 commits ahead.
- `ASSET-NOTES.md` provenance entry present and independently verified
  accurate (see above).
- The pre-existing uncommitted `progress.md` modification was left alone
  throughout this review, as instructed.

## Findings

1. **[Medium — report accuracy]** The draco-round-report's stated
   justification for choosing sample 08 over sample 09 ("`samples/09_scene`
   has no `--scene` flag at all (grep-confirmed)") is factually false —
   sample 09 has a `--scene <path>` flag used extensively for
   Sponza/present-mode. The correct justification (verified independently:
   `runHeadless()`, the function driving 09's actual pixel gate,
   unconditionally hardcodes `resolveHelmetScenePath()` and never reads
   `args.scenePath`) supports the same tool choice but was misstated.
   Non-blocking — the underlying engineering decision is sound — but the
   report text should be corrected.

2. **[Medium — test coverage gap, self-disclosed but understated]** The
   `mapped_index()` bypass (the implementer's own honest-negative,
   Attempt 1) does not discriminate for either committed draco fixture, but
   for a materially different reason than reported: on BoomBox, the same
   bug class causes an out-of-bounds read into Draco's internal attribute
   buffer (`posAttr.size()=3237 < numPoints=3575`, and Draco's
   `GetValue`/`GetAddress` perform no bounds-checking), not a benign
   coincidence — the BoomBox integration test simply never asserts on
   decoded attribute values, only structural counts. This is a real,
   narrow gap in the committed test suite that reaches the round's
   flagship real-world fixture. Does not block this round (criterion 5 is
   satisfied by Attempt 2), but should be closed in a follow-up (a
   value-level assertion on a known BoomBox vertex, or a BoomBox render
   pixel-gate).

No other findings. Nothing else was unverifiable — every claim in the
report that fell within this round's diff was independently reproduced;
Task 13-era claims outside this round's scope (e.g., the encoder
dead-stripping byte-delta measurement) were correctly not re-verified by
either the report or this review, since nothing about those build flags
changed.

---

## Scoped re-review — both Mediums closed (`340c71d`, `019ce14`)

**Verdict: ALL ADDRESSED.**

The implementer's fix closed both Medium findings, and in the process of
investigating Finding 2 surfaced and fixed a genuine, separate, pre-existing
defect in the original decode path (not introduced by this round or the
prior review): `findAttr()` resolved Draco attributes by array position
(`mesh->attribute(attId)`) against ids that are actually Draco's
*persistent* `unique_id()`s — the two coincide only when encode-time
add-order happens to match decode-time array order (true for this
project's own trivial fixtures, false for real-world content, including
BoomBox). On BoomBox this silently returned NORMAL data (unit-scaled,
bounds ≈ [-1,1]) where POSITION was requested (true bounds ≈ [-0.01,0.01]).
All items below independently re-verified; nothing taken on the
implementer's word alone.

1. **Unique_id fix vs. vendored Draco source — CONFIRMED, three-way.**
   `PointCloud::attribute(int32_t)` (`point_cloud.h:78-90`) is a direct
   `attributes_[att_id]` array index; `GetAttributeByUniqueId()` is a
   distinct API (`point_cloud.h:66`). The dep-cache only preserves
   installed headers, not `.cc` sources, so I cross-checked the actual
   *implementation* against an independent copy of the same pinned Draco
   1.5.7 source found elsewhere on this machine (a different project's
   vendored tree, version-verified via `draco_version.h` before trusting
   it): `GetAttributeByUniqueId()` → `GetAttributeIdByUniqueId()` is a
   linear scan comparing `attributes_[att_id]->unique_id() == unique_id`,
   returning `nullptr` on no match (`point_cloud.cc:102-129`) — exactly as
   the fix's own commit message claims. Semantics confirmed correct.

2. **BoomBox min/max test discriminates on lavapipe — CONFIRMED, with the
   exact claimed failure signature.** Reverted `340c71d`'s `findAttr()`
   change in-tree only (kept the bounds-checked-read wrapper), rebuilt,
   ran the BoomBox test on forced lavapipe: the 6 min/max assertions fail
   cleanly (`CHECK` failures, no crash). Added a temporary `MESSAGE` to
   capture the actual corrupted bounds before reverting: **`min=(-1,-1,
   -0.998303) max=(1,1,0.999969)`** — the precise `~[-1,1]` NORMAL-data
   signature the report claims, not an approximation of it. Both edits
   (the `findAttr` revert and the diagnostic `MESSAGE`) reverted
   byte-identically (`git diff` empty) and rebuilt clean before proceeding.

3. **`mapped_index()`-bypass now fails loudly — CONFIRMED, exact message
   match.** Re-applied the same naive-cast bypass from the original
   review's Finding 2 (this time inside `convertAttrChecked`, on top of
   the FIXED `findAttr`), rebuilt, ran BoomBox on lavapipe with
   `--validate`. Output: `[error] rx_asset: Draco decode failed: POSITION
   attribute value index out of range for point 3133 (attribute size
   3133, mesh has 3575 points) -- decoded mesh is internally
   inconsistent`, followed by a clean, non-crashing `REQUIRE` failure
   (`mesh.submeshes.size() == 1` → `0 == 1`) — matches the report's §8.2
   output verbatim, including the exact point index (3133) and attribute
   size (3133/3575). Reverted byte-identically, rebuilt clean, sanity-ran
   all 4 draco/BoomBox cases clean afterward (78/78 assertions).

4. **Full serial ctest, lavapipe: 29/29 passed**, 78.20s, after restoring
   clean state.

5. **Commit hygiene, both commits:** author `Yousef Wadi
   <ywadi85@gmail.com>` (matches local git config), dated 2026-08-20,
   no AI attribution (`git log --format=%B 740fd96..019ce14 | grep -iE
   "claude|anthropic|co-authored|generated with"` empty), nothing pushed
   (`origin/main` still at `11dbc23`, 7 commits behind local `main`).
   Pathspec scope is clean and correctly split: `340c71d` touches only
   `src/rx_asset/import_gltf.cpp` (the fix); `019ce14` touches only
   `src/rx_asset/tests/draco_compression_test.cpp` (the strengthened
   assertions) — no bleed between the two, no stray files.

6. **Report delta accuracy:** §4.1's rewritten 08-vs-09 justification
   matches my own independently-traced `runHeadless()` finding from the
   original review exactly (flag exists on the binary; the specific
   headless/pixel-gate driver never reads it) — Finding 1 is genuinely
   closed at the source, not just patched over.

All temporary edits made during this scoped re-review were reverted
byte-identically at each step; only the pre-existing, coordinator-owned
`progress.md` modification remains in the working tree, untouched.
