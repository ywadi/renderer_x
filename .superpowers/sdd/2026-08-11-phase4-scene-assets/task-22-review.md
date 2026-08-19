# Task 22 review: Shadow quality bridge (D21) + D29 + RC3 reversed-Z migration

Reviewer: independent (did not write this code). Commits reviewed (local,
not pushed): `428ee5d`, `cca7add`, `0cb330e`, `04c50fe` (base `c9ff71c`).
Authority order used throughout: `gate/rulings-2026-08-18.md` §#23 + RC2/RC3
> spec (D13/D21/D26.1/D29) > `gate/matrix-issue23-shadow-bridge.md` > ticket
text (`task-22-brief.md`).

## Verdicts

**Spec compliance: ❌ (partial).** One binding acceptance row is not met
(comparison-sampler PCF is not wired into the scene's real lit path), and a
second is met only partially/vacuously (the depth-clamp regression proves
the override seam, not the pixel-level defect the matrix's own text
requires). Every other binding criterion — D29 per-pass depth convention,
RC3 one-literal compare-op fork with the required non-flip comments,
`VK_DYNAMIC_STATE_DEPTH_BIAS`, opportunistic `depthClampEnable` +
device-feature check, `SV_VulkanInstanceID` bindless addressing, texel
snapping, acne/peter-panning probes, the full reversed-Z main-camera
migration (samples 06/08 + `rx_material`'s own GPU test fixture), and the
D17 bit-exact gate on lavapipe post-migration — is delivered correctly and
is independently re-verified below.

**Code quality: Approved, with findings (none blocking on
architecture/correctness grounds).** The new code is real, RAII-clean,
consistently commented, matches established codebase idioms
(`RX_MATERIAL_SHADER_DIR`-style shader path baking,
`supportsSamplerAnisotropy()`-style opportunistic feature gating, the
warm-up-before-`TEST_CASE` GPU test harness pattern), contains no
stubs/TODOs, and the self-review section in the report is candid rather
than promotional. The findings below are about test-rigor precision and
one missing runtime guard, not functional defects.

## Empirical verification performed

- **Build hazard confirmed and worked around:** `/home/ywadi/d2/renderer_x`
  is a symlink to the real repo (same inode). The shell's logical `$PWD`
  was seeded to the symlinked path, so a naive `cmake --preset
  linux-native` configured against `/home/ywadi/d2/renderer_x`
  (`CMAKE_HOME_DIRECTORY` confirmed via `CMakeCache.txt`) — exactly the
  corrupted-dep-cache-key hazard the task called out. Fixed by `cd -P`
  into the real path before every build/test invocation and
  reconfiguring from scratch (`rm -rf build/linux-native && cmake
  --preset linux-native`).
- **Full clean build**, `linux-native` preset, real path, all targets:
  succeeded (196/196 build steps), including `rx_shadow`,
  `rx_shadow_tests`, `rx_shadow_gpu_tests`.
- **Full ctest run** (`VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a
  ctest --test-dir build/linux-native --output-on-failure -j4`): **26/26
  passed**, 25.20s — matches the report's claimed count exactly (17 new
  library/test targets + samples, `rx_shadow_tests`/`rx_shadow_gpu_tests`
  included).
- **`rx_shadow_gpu_tests --validate` run directly:** 6/6 cases, 60/60
  assertions pass. Confirmed `depthClamp ENABLED` on lavapipe at
  `Device::create`, and confirmed the fallback-logging code path fires
  (`"depthClamp feature not enabled on this device"`) when the
  depth-clamp-off regression variant forces the override — the override
  seam genuinely engages the fallback branch even on a device that
  supports the real feature.
- **Zero unfiltered validation errors**, checked directly: grepped
  `rx_shadow_gpu_tests`/`rx_graph_gpu_tests`/`rx_material_gpu_tests`
  output for `[vulkan validation]` lines NOT tagged `known false
  positive` — zero hits across all three binaries.
- **`rx_graph_gpu_tests`:** 9/9 cases, 633/633 assertions.
  **`rx_material_gpu_tests`:** 50/50 cases, 2379/2379 assertions — both
  match the report.
- **Sample 08 D17 gate, re-run directly on lavapipe:**
  `D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true` and
  `D17 loaded_scene gate: failingPixels=0/65536 (0.0000%) pass=true` —
  bit-exact, confirms the report's claim independently.

### Revert-discrimination re-proofs (two of my own choosing, per the brief)

Both edits were made with `Edit`, rebuilt, run, then restored and verified
byte-identical via `md5sum` + `git diff --stat` (clean) before moving on.

- **D29:** hardcoded `executor.cpp`'s ordinary per-pass depth clear back to
  `VkClearDepthStencilValue{1.0F, 0}` (bypassing `depthClearValueFor()`).
  Rebuilt `rx_graph_gpu_tests`, ran `--test-case="D29*"`: failed exactly as
  predicted — `CHECK( static_cast<int>(reversedRed) == 0 ) is NOT
  correct! values: CHECK( 255 == 0 )`, 1/19 assertions failing. Restored;
  `md5sum` matched the pre-edit hash.
- **RC3:** reverted `material_system.cpp`'s `depthCompareOp` to
  `VK_COMPARE_OP_LESS`. Rebuilt `rx_material_gpu_tests`, ran the full
  suite: **16/50 cases failed, 44/2379 assertions failed** — the exact
  signature the report claims. Restored; `md5sum` matched.
- Working tree after both reverts+restores: `git status` shows only the
  pre-existing `progress.md` modification (untouched by this review, as
  instructed).

## Adjudications (the three items called out for this review)

### 1. Comparison-sampler PCF not integrated into `forward_entry.slang`

**Ruling: this is a genuine spec/matrix gap — ❌ on this criterion,
independent of the (sound) engineering rationale.**

Quoting the rows I ruled on:

- `task-22-brief.md`, base file list (pre-gate-hardening): *"3×3 PCF in
  the standard lit path (`shaders/material/forward_entry.slang` shadow
  helper upgrade; sample 05 keeps its own simpler shaders untouched —
  documented)."*
- The same file's BINDING gate-hardening delta: *"Key deltas: FILE LIST
  **GROWS** — `src/rx_graph/{resources.h,executor.cpp}` ... shadow-caster
  pipeline is built OUTSIDE MaterialSystem ... comparison-sampler PCF
  (compareEnable=TRUE, COMPARE_OP_LESS, SampleCmp-equivalent taps —
  hardware filtering, not sample 05's manual compare) ..."* — this
  enumerates *additions* to the base file list; nothing in it removes or
  narrows the base list's own `forward_entry.slang` line.
- `gate/matrix-issue23-shadow-bridge.md`, "Comparison-sampler PCF" row,
  Proposed acceptance criterion: *"the shadow-sampling helper for the
  scene path creates and uses a `VkSampler` with `compareEnable=VK_TRUE`,
  `compareOp=VK_COMPARE_OP_LESS` ... and the 3×3 PCF loop's 9 taps each
  read the hardware-filtered comparison result directly ... **rather than
  manually comparing a raw depth sample as `lit.frag.slang` does
  today**."* — the explicit contrast is against sample 05's *shading*
  path, which only `forward_entry.slang`/StandardPBR (the actual "scene
  path" shader, already delivered and consumed by real samples 06/08)
  can supersede for production draws.
- `gate/rulings-2026-08-18.md` §#23: *"Comparison-sampler PCF adopted
  (compareEnable=TRUE, COMPARE_OP_LESS, SampleCmp-equivalent taps —
  fast-path-as-default)."* — adopts the mechanism; does not narrow or
  except it to a standalone rig.
- Spec D21: *"Phase 4 **ships** the production-credible single-map
  baseline: light frustum fitted to the visible scene, slope-scaled depth
  bias, 3×3 PCF."* — present-tense, this-phase delivery, no forward
  reference to Task 24.

None of the higher-authority sources (rulings > spec > matrix) narrow or
except the base ticket's explicit `forward_entry.slang` line, and the
gate-hardening delta explicitly frames itself as additive ("FILE LIST
GROWS"). The delivered work proves the *mechanism* (real
`compareEnable=TRUE`/`COMPARE_OP_LESS` sampler, real hardware `SampleCmp`,
9 taps) genuinely and rigorously in `rx_shadow`'s own GPU probe rig — but
that rig has its own bespoke receiver shader that no production material
ever calls; `shaders/material/forward_entry.slang` is unmodified by this
task (confirmed: absent from the diff's file list entirely). The report's
own "Deviations" section makes a reasonable *engineering* case for
deferral (no real shadow-casting scene-path consumer exists yet before
Task 24/sample 09), but that is a scope decision the implementer made
unilaterally after the fact, documented in a report — not something
authorized by any binding gate/spec/matrix text. Per this review's
instructed standard, the engineering rationale does not convert a missed
binding criterion into a satisfied one.

### 2. Depth-clamp regression: mechanism/override seam vs. pixel-level repro

**Ruling: insufficient per the matrix row's own wording.**

Matrix text: *"a regression variant with clamp disabled demonstrates the
defect this setting fixes (missing/truncated shadow), **proving the test
actually exercises the setting rather than passing vacuously**."*

The delivered test (`test_shadow_caster_gpu.cpp`,
`"Depth clamp regression: ..."`) builds two pipelines
(`depthClampOverride=true/false`) and asserts only
`clampOn->depthClampEnabled == true` / `clampOff->depthClampEnabled ==
false` — i.e., it checks that the pipeline object correctly echoes back
the override it was given. It does not render, does not read back
pixels, and — by its own code comment — **the two variants produce
identical rendered output in this scene** ("a caster fully within the
padded depth range is unaffected either way for THIS scene"). This is
precisely the vacuous-test shape the matrix's own text was written to
rule out: a config-accessor round-trip check is a strictly weaker claim
than "demonstrates the defect this setting fixes." The report is honest
about this gap (its own Deviations §3), which is to its credit, but
honesty about a gap doesn't close it. The scene bound (`fitShadowFrustum`
padded generously) was the implementer's own construction — a
deliberately tight/unpadded probe scene with a caster placed beyond the
tight near plane was an available, buildable alternative within this
same rig and was not attempted.

### 3. New `rx_shadow` library vs. the brief's original file list

**Ruling: legitimate structural choice within scope, not scope creep.**

The BINDING gate-hardening text (RC3) *requires* the shadow-caster
pipeline be *"built OUTSIDE MaterialSystem"* — this mandates new code
that cannot live in `rx_material`, superseding the pre-hardening brief's
one-line file list (*"Modify `shaders/multipass/` shadow path shared
pieces as needed"*), which predates the 2026-08-18 gate ruling that
substantially re-scoped this ticket (RC2 widened it into `rx_graph`; RC3
mandated an independent pipeline). Given this codebase's own established
convention — one `rx_*` static library per cohesive subsystem
(`rx_graph`, `rx_material`, `rx_scene`, `rx_debug_ui`, each with its own
`CMakeLists.txt`/`tests/`/`include/`) — and given the registry explicitly
anticipates this mechanism being extended again in the cascades-phase
work (registry item 13: *"Shadow-map resolution/format policy tiers ...
cascades work inherits an explicit 1024/D32_SFLOAT default"*) and reused
by Task 24's real scene-path sample, packaging the "outside
MaterialSystem" pipeline plus its device-free fitting math as a standalone
library is the natural, idiomatic realization of scope RC3 already
mandated — not an expansion of it. `rx_shadow`'s own `CMakeLists.txt`
correctly does not link `rx_material` (verified), matching RC3's intent.

## Findings

**[HIGH] F1 — Comparison-sampler PCF not wired into the real scene lit
path.** See Adjudication #1. `shaders/material/forward_entry.slang` is
unmodified; the mechanism is proven only in `rx_shadow`'s own standalone
probe rig with a bespoke, non-production receiver shader. This is the
primary driver of the ❌ spec-compliance verdict.

**[MEDIUM] F2 — Depth-clamp regression test is not the pixel-level proof
the matrix requires.** See Adjudication #2. Asserts a config accessor,
not rendered output; the test's own comment concedes both variants render
identically in this scene.

**[MEDIUM] F3 — Vacuous test title/body mismatch:
`"Slope-scaled depth bias is genuinely wired: two different bias values
produce measurably different shadow-map depths"`
(`test_shadow_caster_gpu.cpp`).** The body runs `runProbe` twice with
different bias values but never measures or compares a shadow-map depth,
a pixel value, or any numeric output — its only assertion is
`CHECK_FALSE(fixture->context.hasValidationErrors())`. It would pass
identically if depth bias were a complete no-op. The underlying claim
("bias is genuinely wired") *is* proven elsewhere, honestly, by the
acne-probe test's biased-vs-unbiased variance comparison — this specific
test case's own name and its own header comment both promise a
measurement its body does not perform. Should be deleted (the acne probe
subsumes it) or rewritten to actually compare sampled shadow-map depths
between the two bias configurations.

**[MEDIUM] F4 — D29 GPU test does not literally implement "a two-pass
frame mixing both conventions" (rulings §#23/RC2's own wording).** The
delivered test runs two *separate* single-convention graphs
(`compile()`/`realize()`/`execute()` called independently per convention)
rather than one graph with two depth-bearing passes of differing
convention executing within the same `execute()` call. It also only
empirically exercises the ordinary per-pass clear site (`executor.cpp`
~:1141); the pinned-history init-clear site (~:646) is asserted correct
for the `Reversed` case only by code-sharing/inspection (both sites call
the same `depthClearValueFor()`), not by an independent GPU-observed
result — the report's per-criterion table implies both sites are
empirically proven, which overstates what the test actually exercises.
Residual risk is low (the function is a pure per-attachment read, no
shared mutable state), but the letter of the ruling's named test
methodology is not met.

**[LOW] F5 — `ShadowCasterPipeline::create()`/destruction lack the
codebase's `RX_ASSERT_MAIN_THREAD` runtime guard.** The class's own
header comment claims *"matching rx::material::MaterialSystem's own
established pattern"* for main-thread-only lifetime — but
`MaterialSystem::create/loadMaterial/bindInstance/reloadChanged/
getPipeline` and `Device::create`/`present`/etc. all carry
`RX_ASSERT_MAIN_THREAD(...)`; `rx_shadow` has zero occurrences of this
macro anywhere. The documented thread contract is not runtime-enforced,
unlike the precedent it claims to mirror.

**[LOW] F6 — Minor claim overstatement: "BIT-IDENTICAL" texel-snapping
proof.** The report and the test's own header comment describe the
sub-texel-shift invariance test as producing a "BIT-IDENTICAL"
`lightViewProj`; the actual assertion uses
`doctest::Approx(...).epsilon(1e-6)`, not exact `==`. The epsilon is tight
enough that this doesn't weaken the discrimination (a real sub-texel vs.
multi-texel shift is trivially distinguishable at that tolerance), but
the word "bit-identical" is not literally what's checked.

## Commit hygiene

- Author/committer on all four commits: `Yousef Wadi
  <ywadi85@gmail.com>` — matches the configured git identity, confirmed
  directly via `git show -s --format='%an <%ae> / %cn <%ce>'` per commit
  (not merely trusted from the report).
- `git log -4 --format=%B 428ee5d..04c50fe | grep -i
  "claude\|anthropic\|co-authored\|generated by\|AI assist"` — zero hits.
- Pathspec scope matches each commit's own message exactly: `428ee5d`
  touches only `rx_graph` (executor/resources/tests); `cca7add` touches
  only `rx_material` + the two migrated samples (06/08) + its own GPU
  test fixture; `0cb330e` touches only new `rx_shadow` files + the
  top-level `CMakeLists.txt` `add_subdirectory` line + `device.h/.cpp`
  (the depth-clamp feature accessor) + the new shader file; `04c50fe`
  touches only the report doc.
- Nothing pushed: `main` is `ahead 4` of `origin/main`; no push
  performed by this review.
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md`'s
  pre-existing working-tree modification was left untouched throughout
  (confirmed via `git status` before and after every temporary edit made
  during this review).

## Not independently verifiable

- The Steam Deck / non-lavapipe local-driver numbers quoted in the report
  (NVIDIA divergence figure) were not re-run — no such hardware available
  in this environment; not required by this task's exit criteria (D17's
  own policy: local-driver divergence is informational only).
- `windows-cross-zig`/Wine GPU pass-through results were not re-run (the
  brief's empirical list only calls for the linux ctest suite + D17 gate
  + rx_shadow/rx_graph tests, all of which were reproduced above).
- Whether a genuinely near-plane-crossing pixel-level depth-clamp repro
  is achievable within this rig's existing scene bounds without further
  rework was not built/attempted by this review (would require
  constructing a second, deliberately-tight-fit scene); flagged as an
  open question in F2/Adjudication #2 rather than resolved.

---

## Scoped re-review (fix round): F1–F6

Package: `review-04c50fe..4e0f271.diff`, 7 commits
(`7dd5aaa, e2c22a6, a8e6438, 7b0bba1, 417d440, 4e0f271` + `de7ba54`).
`de7ba54` ("docs: registry -- Bistro committed as the techniques-phase
showcase/benchmark scene") touches only
`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md` (11
lines) — confirmed coordinator-owned and out of scope, excluded from
everything below. Scope re-verified directly per commit via `git show
--stat`, not merely trusted from the report.

### Per-finding verification

**F1 [was HIGH, spec ❌] — ADDRESSED.** Confirmed by reading the actual
shader/binding wiring, not just the report's description:
- `shaders/material/material.slang` now declares
  `[[vk::binding(3, 0)]] SamplerComparisonState gShadowCompareSamplers[]`
  and a real `rx_sampleShadowPCF()` — 9 genuine `Texture2D.SampleCmp()`
  hardware-comparison taps, no manual `if (depth > stored)` anywhere.
  `forward_entry.slang`'s `fragmentMain` calls it and multiplies
  `lightColor` (direct term only, not ambient) by the result — this is
  the real, shipped `StandardPBR` path every material using
  `forward_entry.slang` now runs through, not a parallel rig.
- `rx::rhi::BindlessTable` gained a real fourth, optional binding
  (`ComparisonSampler`/`kComparisonSamplerBinding=3`,
  `Capacities::comparisonSamplers`), wired through
  `PipelineLayoutBuilder`'s independent validation path AND
  `MaterialSystem::reflectMaterialLayout()`'s separate one (both were
  genuinely necessary — confirmed via the report's own "Errors and
  fixes" account of hitting the second one, which is consistent with
  reading Slang's reflection `Kind` giving no compile-time discriminator
  between `SamplerState`/`SamplerComparisonState`).
- Backward compatibility verified, not assumed: `DrawDataGpu`/`RxDrawData`
  both default `shadowMapTextureIndex = 0xFFFFFFFFu` (the "no shadow"
  sentinel), and `rx_sampleShadowPCF()` returns `1.0` immediately for
  that sentinel without touching either bindless array — samples 06/08
  correctly add `comparisonSamplers=1` capacity (required for the
  descriptor-set-layout shape to match the SPIR-V, since Slang doesn't
  dead-strip the new global) but leave their `DrawDataGpu` rows at the
  sentinel default, so no rendering change is expected there. This
  matches the sample08 D17 re-run below.
- `src/rx_material/tests/test_standard_pbr_shadow_gpu.cpp` (new, 1020
  lines) builds its scene through `rx::material::MaterialSystem::create()`
  → `loadMaterial("standard_pbr.slang", ...)` → `getPipeline()` —
  confirmed directly by reading the fixture code, not the standalone
  `rx_shadow` receiver shader. Its acne/peter-panning/PCF-softness
  assertions are relative comparisons (biased-vs-unbiased variance,
  contact-vs-reference brightness, texel-gradient span), not fixed
  loose thresholds — not vacuous on inspection.
- The "no classic revert" claim is accepted: this is new coverage of
  previously nonexistent code, so there is no pre-fix passing state to
  regress against — a structurally different but legitimate class of
  evidence from the D29/RC3-style reverts, and the report is honest
  about the distinction rather than fabricating a revert. Independently
  re-ran `rx_material_gpu_tests`: **53/53 cases, 2515/2515 assertions**,
  matching the report exactly.
- **Spec-compliance verdict on this row flips to PASS.** The binding
  criterion (comparison-sampler PCF wired into the scene's real lit
  path, superseding `lit.frag.slang`'s manual compare) is now
  demonstrably met by the actual shipped `forward_entry.slang`/
  `material.slang` code, not merely asserted.

**F2 [was MEDIUM] — ADDRESSED, confirmed non-vacuous.** The regression
now uses `buildClampTestGeometry()` (a pole whose top corners are
numerically worked out to sit beyond the fitted near plane while the
base/ground stay in range) and asserts on RENDERED pixels:
`farOn < 96` (clamp ON keeps the far reach shadowed) and
`farOff > farOn + 64` (clamp OFF measurably brightens the same point —
the defect), with `nearOn`/`nearOff`/`refOn`/`refOff` as unaffected
controls. **Spot-checked myself**, independent of the report's own
revert notes: forced `rasterizationState.depthClampEnable = VK_FALSE`
unconditionally in `ShadowCasterPipeline::create()`, rebuilt
`rx_shadow_gpu_tests`, ran the depth-clamp test case — failed exactly as
the mechanism predicts (`farOn=216`, `farOff=216`, so
`CHECK(farOff > farOn + 64)` → `216 > 280` is false; two assertions
failed). Restored; `md5sum` confirmed byte-identical
(`c4944a2525c4c4fb3e40efd4011897fe`, matching pre-edit). This is now a
real, discriminating, pixel-level proof matching the matrix's own
"demonstrates the defect ... not vacuously" wording.

**F3 [was MEDIUM] — ADDRESSED.** The bias-wiring test now does a direct
`vkCmdCopyImageToBuffer` raw-depth readback of the shadow map at a known
texel and asserts `*biasedDepth > *unbiasedDepth` (plus both-in-range
checks) — a real numeric comparison backing the test's own title, not
merely "no validation errors." Confirmed by reading the diff directly.

**F4 [was MEDIUM] — ADDRESSED, and more thoroughly than strictly
required.** Rebuilt as ONE `RenderGraph`/ONE `Executor::execute()` call
with `depthStandard`+`depthReversed` (the ordinary per-pass site,
genuinely mixed within one frame) plus a `historyWrite` pass
establishing a Reversed-convention pinned-history resource, probed via
`addHistoryInput()` — proving the previously-unexercised pinned-history
init-clear call site (`executor.cpp`'s `initializePinnedHistoryEntry()`)
independently, in the same frame, via `ctx.historyValid()==false` +
slot-0 readback. This closes both parts of the original F4 finding (the
"one frame" literalism AND the pinned-history coverage gap) in a single
test. Re-ran `rx_graph_gpu_tests` myself: **9/9 cases, 635/635
assertions**, matching the report.

**F5 [was LOW] — ADDRESSED.** `RX_ASSERT_MAIN_THREAD("ShadowCasterPipeline::create")`
and the matching destructor guard are present, confirmed by direct read
of `shadow_caster_pipeline.cpp` — now matches the precedent (`Device::create`,
`MaterialSystem`'s own methods) the class's header comment already
claimed to mirror.

**F6 [was LOW] — ADDRESSED.** `test_shadow_frustum.cpp`'s two header
comments now read "identical to float round-off (doctest::Approx
epsilon=1e-6 -- not bitwise ==)" in place of "BIT-IDENTICAL" — confirmed
by direct diff read; the underlying assertion and discrimination are
unchanged.

### Empirical verification (fix round)

- Full clean-tree build (real path, `linux-native`) succeeds; confirmed
  the built binaries actually contain the new code via `strings` grep
  for new test-case names before trusting any run.
- `rx_material_gpu_tests --validate`: **53/53 cases, 2515/2515
  assertions**, zero unfiltered validation errors — matches the report.
- `rx_graph_gpu_tests --validate` (D29 binary): **9/9 cases, 635/635
  assertions**, zero unfiltered validation errors — matches the report.
- `rx_shadow_gpu_tests --validate`: **6/6 cases, 74/74 assertions**,
  zero unfiltered validation errors — matches the report.
- Full `ctest` (real path, lavapipe, `xvfb-run`): **26/26 passed**.
- Spot-checked revert: F2 (depth-clamp regression), see above — genuinely
  discriminating, restored byte-identical.
- Commit hygiene (6 finding-owning commits, `de7ba54` excluded): all
  authored/committed as `Yousef Wadi <ywadi85@gmail.com>`; zero AI
  attribution hits (`grep -i "claude\|anthropic\|co-authored\|generated
  by\|AI assist"` across all 6 messages); each commit's touched-file set
  matches its own commit message/finding exactly (`7dd5aaa`→
  `rx_rhi_vk` bindless/pipeline-layout only; `e2c22a6`→ `rx_material` +
  the two migrated samples + shaders; `a8e6438`→
  `test_shadow_caster_gpu.cpp` only; `7b0bba1`→
  `shadow_caster_pipeline.cpp` only; `417d440`→ `test_execute_gpu.cpp`
  only; `4e0f271`→ the report + `test_shadow_frustum.cpp`); nothing
  pushed (`ahead 11` of `origin/main`); pre-existing `progress.md`
  modification untouched throughout (confirmed via `git status` before
  and after the F2 spot-check edit).

### Final verdict

**ALL SIX FINDINGS ADDRESSED.** F1–F6 each independently confirmed by
reading the actual code (not just the report's narration), rebuilding,
and — for F1/F2/F4 — either re-running the new tests directly or
personally reproducing a genuine revert-discrimination failure.

**Spec-compliance verdict flips: ❌ (partial) → ✅ PASS.** The original
❌ rested on two rows: (1) comparison-sampler PCF not reaching the real
scene lit path — now closed, `forward_entry.slang`/`material.slang`
genuinely run hardware `SampleCmp` PCF in the shipped `StandardPBR`
path, proven by a new production-pipeline test suite (53/53,
2515/2515); (2) the depth-clamp regression being a config-accessor
check rather than a pixel-level defect proof — now closed, replaced by
a numerically-derived near-plane-crossing scene with a real rendered-
pixel discrimination, independently spot-checked and confirmed
load-bearing by this review. No other binding criterion from the
original review was ever in question. With both rows closed and no new
gaps introduced (D29/RC3/D17/full-suite regressions all re-verified
clean), the task's spec-compliance verdict for Task 22 is **PASS**.

**Code-quality verdict: Approved.** The fix-round code (new
`ComparisonSampler` bindless kind, shader wiring, geometrically-derived
test scenes, `RX_ASSERT_MAIN_THREAD` guards, wording correction) is
consistent in style and rigor with the rest of this codebase; no new
findings identified during this scoped re-review.
