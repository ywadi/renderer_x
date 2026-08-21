# Task 9 review — compute IBL bake chain (issue #45)

**STATUS: SUPERSEDED BY THE RE-CHECK AT THE END OF THIS FILE.** The
original round below found spec compliance FAIL; a scoped re-check of
commits `267e55a`+`23fb276` closed that gap and all 3 quality findings.
Current verdict: **spec compliance PASS, code quality Approved** — see
"Re-check" at the bottom. The original text is left in place, unedited,
per this project's disclose-honestly norm (the finding and its
resolution are both part of the record).

Independent reviewer round. Commits under review: `d0ec616` (feat, rx_ibl
module + shaders/ibl/*.slang + tests + bench), `3299993` (chore, SDD
report). Base `60531d1`. Reviewer did not write this code; every finding
below is from direct re-execution, a fresh fetch of the pinned Filament
source, or a direct read of the ticket/plan text — not from trusting the
implementer's or the report's own characterizations. No forks or
subagents were used anywhere in this review (the fork-overstep incident
disclosed in the report and independently corroborated in
`progress.md` makes this an absolute rule for this round); all
verification below was run directly by the reviewer, serially, in the
foreground, solo on the shared GPU.

## Verdict 1 — Spec compliance: **FAIL** (matrix-p5t09, as amended by the
T9 ruling) — one binding acceptance-sketch requirement not built, and
not covered by any "OR" the source text actually contains
**[SUPERSEDED — see Re-check: now PASS]**

Everything in the T9 per-ticket ruling itself (irradiance-cubemap
baseline, Filament v1.75.0 `libs/ibl` pin, compute-based execution
through Task 2's machinery) is satisfied and independently verified.
Five of the six acceptance-sketch micro-requirements are not just met
but exceeded (exact closed-form proofs where the ticket only asked for
value probes). The sixth is not built, and the report's own
justification for treating that as acceptable rests on a mischaracterization
of the source document — see the OR-adjudication section below, which is
the deciding factor in this verdict. Scope of the failure is narrow and
precisely bounded; it is not a global judgment on the round's quality
(see Verdict 2).

## Verdict 2 — Code quality: **Approved, with 1 MEDIUM and 2 LOW findings**
(no correctness blockers)

Every ported formula was independently checked against a fresh fetch of
the pinned Filament commit and matches exactly (see Port fidelity
below). Both drivers are validation-clean, the sabotage/revert
reproduction below confirms the tests actually discriminate against a
realistic bug class, and the seven disclosed in-round bug fixes are all
real, all reproduced in code, and all pinned by a passing test. The
MEDIUM finding is a stale public-API doc comment that contradicts the
shipped (and correctly disclosed) design; the two LOW findings are
citation/documentation nits with no behavioral effect.

---

## Reproduction log (all commands re-run independently this session)

**Full ctest suite, lavapipe, serial, foreground — CONFIRMED 33/33
(100%).** `VK_ICD_FILENAMES=lvp_icd.json xvfb-run -a ctest --preset
linux-native --output-on-failure -j1`, driver-labeled llvmpipe/Mesa.
Includes `rx_ibl_gpu_tests` and every pre-existing suite (`rx_graph_gpu_tests`
etc.) — no regression in Task 2's shared machinery.

**`rx_ibl_gpu_tests`, real NVIDIA — CONFIRMED, driver-labeled (`NVIDIA
GeForce RTX 2080`, driver `580.82.07`, default ICD, confirmed via
`nvidia-smi`/`vulkaninfo` this session, no `VK_ICD_FILENAMES` override):**
```
[doctest] test cases:   6 |   6 passed | 0 failed | 0 skipped
[doctest] assertions: 318 | 318 passed | 0 failed |
```
Exact match to the report's `6/6`, `318/318`. `grep "Validation Error" |
grep -v "known false positive" | wc -l` → `0` on this run's full log.
Matches the report's zero-unfiltered-errors claim.

**Timing-methodology reproduction, real NVIDIA — CONFIRMED.** Cleared
`/tmp/rx_ibl_bake.cache`, ran `rx_ibl_bench` twice at the report's own
production-scale parameters:
```
run 1 (cold pipeline cache): equirect=1.001ms irradiance=0.496ms prefilter=1.110ms dfg=0.422ms total=299.595ms
run 2 (warm pipeline cache): equirect=1.023ms irradiance=0.430ms prefilter=0.945ms dfg=0.368ms total=299.401ms
```
Per-stage GPU cost is sub-4ms combined on both runs; total barely moves
(299.6ms → 299.4ms) despite the warm on-disk `VkPipelineCache`,
independently confirming the report's claim that the ~300ms `total` is
dominated by in-process Slang front-end compilation, not GPU work or
`vkCreateComputePipelines`. Consistent with the report's own NVIDIA
numbers (306ms/302ms) to within normal run-to-run variance.

**Revert/sabotage reproduction (reviewer's choice: asymmetric GGX
importance-sample weighting) — CONFIRMED the tests discriminate against
a realistic bug.** Edited `shaders/ibl/prefilter_specular.slang`'s
accumulation loop from
```
accum += gSourceCube.SampleLevel(gSourceSampler, l, 0.0).rgb * noL;
weight += noL;
```
to `weight += noL * noL;` (numerator and denominator use different NoL
exponents — a realistic real-world weighting-bug shape, not a
degenerate no-op). Re-ran `rx_ibl_gpu_tests` (source recompiles from
disk at runtime, no C++ rebuild needed) on real NVIDIA:
```
[doctest] test cases:   6 |   4 passed | 2 failed | 0 skipped
[doctest] assertions: 318 | 310 passed | 8 failed |
```
Failures land exactly where expected: the uniform-environment exact-
conservation test (7 assertions, every prefiltered-mip RGB channel) and
the non-uniform monotonicity test (1 assertion). Reverted the edit;
`git diff --stat` / `git status --porcelain shaders/ibl/` both empty —
byte-identical restore confirmed.

Also traced (analytically, not by a second live edit, given time
budget) what this same reproduction implies about **symmetric**
weighting bugs and pure **direction**-formula bugs — see the
OR-adjudication section, which is where this matters for the verdict.

**Commit hygiene — CONFIRMED.** Two commits only (`d0ec616` feat,
`3299993` chore), both authored/committed by `Yousef Wadi
<ywadi85@gmail.com>`. `git log --oneline -- shaders/ibl/ src/rx_ibl/`
shows exactly one commit (`d0ec616`) touching either path — no orphaned
fork content, no stray commits in the collision zone. `git status
--porcelain` is clean except the pre-existing `.superpowers/.../progress.md`
modification (present before this round, independently corroborates the
report's own incident account, left untouched per instructions — not
this task's file to touch and not part of either commit under review).
`git diff 60531d1..3299993 | grep -iE "claude|anthropic|co-authored|generated by"`
finds one hit, a benign "was generated by this task ... via
`stbi_write_hdr()`" sentence describing the HDR test-fixture's own
provenance, not AI attribution. Two commits ahead of `origin/main`,
nothing pushed (`git status -sb` → `ahead 2`).

---

## Port fidelity — re-fetched Filament v1.75.0 directly, cross-checked
line-for-line

Fetched `libs/ibl/src/CubemapIBL.cpp` (1040 lines),
`libs/ibl/src/CubemapUtils.cpp`, and `libs/ibl/include/ibl/Cubemap.h`
fresh via `gh api ... ?ref=0e58877c09afb1aacd09ff640f74d2adcd2a7e80`
(the pinned commit) — not trusted from the report's or matrix's own
quotes.

- **`hemisphereImportanceSampleDggx`/`hemisphereCosSample`**: byte-for-byte
  match (`CubemapIBL.cpp:50-65` vs. every kernel's own duplicated copy).
- **`Visibility()`** (`:170-177`): exact match; the port's added
  `max(ggxV+ggxL, 1e-5)` guard is a disclosed, harmless fp32 div-by-zero
  hardening (CPU original has no such guard; not needed at fp64/CPU
  precision).
- **`getDirectionFor()`** (`Cubemap.h:163-177`) and the `Face` enum
  (`PX=0,NX=1,PY=2,NY=3,PZ=4,NZ=5`, `Cubemap.h:62`): every one of the six
  per-face formulas matches exactly, confirmed the standard Vulkan
  `VK_IMAGE_VIEW_TYPE_CUBE` layer order is what Filament's own CPU
  convention already uses.
- **`toRectilinear()`** (`CubemapUtils.cpp:186-192`): the
  direction↔equirect-UV formula (`atan2(x,z)/PI`, `asin(y)*2/PI`) matches
  exactly; the pixel-space scaling the CPU original applies (its own
  hand-indexed buffer fetch) is correctly absent from the GPU port, which
  samples a hardware sampler at normalized UV instead — a real, disclosed
  simplification, not a missed formula.
- **`DFV_Multiscatter()`** (`:790-833`) and **`CubemapIBL::DFG()`**
  (`:1008-1037`): exact match, including the `(x,y)↔(NoV,linearRoughness)`
  texel parameterization.
- **Re-derived the DFG closed-form limit independently** (not copied from
  the report): at `linearRoughness=0`, `hemisphereImportanceSampleDggx`
  collapses to `H=(0,0,1)` for every sample (the `cosTheta2` formula
  reduces to `(1-u.y)/(1-u.y)=1`), forcing `VoH=NoV`, `NoL=NoV`, `NoH=1`;
  `visibilityTerm(NoV,NoV,0)` reduces algebraically to `0.25/NoV²`, so
  every sample's `term = v*NoL*(VoH/NoH)` collapses to the constant
  `0.25` independent of `NoV`. Summing and scaling by `4/N` gives
  `DFV_Multiscatter(NoV,0) = ((1-NoV)^5, 1.0)` exactly — matches the
  report's own §2 derivation and matches what the real-driver test run
  above confirms empirically.
- **`roughnessFilter()`** (`:296-465`): the port's `accum += color*noL;
  weight += noL;` online running sum is algebraically identical to the
  CPU original's two-pass `cache`-then-`entry.brdf_NoL *= 1/weight`
  structure (same numerator/denominator, computed in a different but
  equivalent order). One documentation-only mismatch found here — see
  Finding 2 (LOW) below.
- **`diffuseIrradiance()`** (`:554-634`): the `Ed()=(1/N)*sum L(l)`
  estimator and the tangent-basis construction (`up`/`tangentX`/`tangentY`)
  match exactly.

**No silent formula drift found anywhere in the four kernels.** Every
deviation from the literal CPU source (dropped mip-chain LOD-biasing,
one hardware bilinear sample instead of Hammersley-jittered
supersampling, no per-scanline sample-set rotation) is disclosed in the
shipped file's own header comment and is a variance-reduction/perf
optimization on top of the estimator, not a correctness requirement of
it — consistent with what the report claims.

---

## OR-adjudication — the deciding factor in Verdict 1

The report's "Scope note" and the coordinator's own `progress.md` entry
both frame the gap the same way: *"the matrix's acceptance sketch offers
'uniform white environment... OR directional impulse' — the
uniform-environment case is the one this task built... to a stronger
(exact, not approximate) standard than the ticket's own minimum bar."*

**This "OR" does not exist in either binding source document.** Fetched
both directly this session:

- Plan (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:384-386`):
  *"SH/irradiance VALUES asserted against analytic ground truth (uniform
  white environment → known coefficients/irradiance; directional impulse
  → known lobe)."*
- Ticket (`gh issue view 45`, "Key acceptance criteria (directional —
  hardened by the Stage 0 primary gate)"): **identical wording**,
  semicolon-joined, same parenthetical.

Both instances use `;`, not "or"/"either", and the very next
acceptance-sketch bullet uses the identical semicolon-list convention
for three items that are unambiguously all-required ("mip-0 ≈ source;
highest-roughness mip ≈ irradiance...; DFG LUT spot values match..." —
all three were built, none treated as alternatives to each other). Read
in that same convention, "uniform white environment → known
coefficients/irradiance; directional impulse → known lobe" is two
required proofs under one bullet, not a disjunction. Nothing in the T9
ruling (`rulings-2026-08-20.md`, per-ticket T9 entry) touches this
acceptance line at all — it only rules on the SH9-vs-irradiance-cubemap
question and the Filament pin, so it does not narrow or rescue this
requirement.

**Does the exact-closure standard actually subsume what the impulse test
would have proven?** No, and the gap is concrete, not theoretical.
Empirically characterized this via the sabotage reproduction above and a
static read of the fixtures:

- The uniform-white test (`test_ibl_analytic_gpu.cpp`, §1) is
  **architecturally content-invariant to direction** — every face/sample
  sees the identical input value, so any weighting formula that keeps
  numerator and denominator *proportional* (including a uniformly wrong
  one) still collapses to the same constant. The sabotage above shows it
  *does* catch an asymmetric numerator/denominator mismatch, because that
  specific bug shape breaks the proportionality — but it provably cannot
  catch a bug that scales GGX lobe width/shape *consistently* (e.g. a
  wrong `alpha`↔roughness mapping, or a uniformly-mis-scaled cone angle),
  since the algebra `result = L0 * sum(w_i)/sum(w_i) = L0` holds for
  **any** single weight function `w_i`, correct or not.
- The non-uniform monotonicity test (`test_ibl_analytic_gpu.cpp`, §3,
  5-dark/1-bright-face fixture) is real and does exercise
  roughness-dependent blur — but it only checks a single scalar
  ordering property (brightness at one face's center is
  non-increasing across mips, with `>0.1` net decrease) at **one**
  fixed direction. It has no way to detect whether the lobe's angular
  *width* or *shape* at a given roughness is quantitatively correct
  (a lobe that blurs "too little" or "too much" by a constant factor
  at every mip still produces a monotonically-decreasing, real-net-decrease
  sequence and would pass this test unmodified).
- `test_ibl_cube_face_convention_gpu.cpp` is a genuine, GPU-verified
  hardware round-trip and is the one test that reliably catches
  face-order/handedness bugs — but it tests discrete face *indexing*,
  not the continuous angular falloff a "directional impulse → known
  lobe" proof would have verified.

So: no committed test would catch a class of bugs in the GGX lobe's
roughness-to-angular-width mapping that preserves per-step monotonic
ordering and preserves the uniform-environment integral — exactly the
"direction-flipping bug that preserves integrals" class this review was
asked to consider. This is a real, previously-unnamed coverage gap, not
merely a documentation shortfall.

**Ruling:** the row is **not satisfied as written** — it required two
proofs, one was built (to a genuinely stronger standard than asked,
which is creditable), the other was not, and the report's framing of
this as satisfying a disjunctive alternative is not supported by either
source document. The omission itself was disclosed honestly (it is not
hidden — it is a headline "Concern for the coordinator" in the report),
which is why this verdict is scoped narrowly to this one row rather than
read as a broader integrity problem. The straightforward remedies are:
(a) build the directional-impulse closed-form test in a follow-up round
(the report's own stated reason for not attempting it — an
error-prone hemisphere-overlap integral without a second source to
check against — is a legitimate engineering judgment call, not a
throwaway excuse), or (b) have the coordinator issue an explicit ruling
narrowing this acceptance line, the same way the T9 ruling already
narrowed the SH9-vs-cubemap question, rather than leaving it to be
resolved unilaterally by an implementer's own re-reading of "; " as
"or".

---

## Findings

1. **[MEDIUM]** `src/rx_ibl/include/rx_ibl/bake.h`'s own
   `bakeEnvironment()` doc comment states: *"Runs the full bake chain as
   a SINGLE render-graph compute-pass graph (one
   compile()+realize()+execute() call, one command-buffer submission)."*
   This is factually wrong against the shipped implementation: `bake.cpp`
   runs **four** separate `RenderGraph`s (`baseGraph`, `irradianceGraph`,
   `prefilterGraph`, `dfgGraph`), each with its own `compile()`/`realize()`/
   `execute()`/`runOnce()` — exactly as the report's own "Design decision:
   four graphs, not one" section describes and justifies (a real,
   deliberate, disclosed design correct for the subresource-validator
   reason it names). The header comment appears to be a stale artifact
   from an earlier single-graph design that was abandoned during
   implementation and never updated. This is the ONLY public doc surface
   a future integrator (Task 10) is likely to read without also reading
   `bake.cpp`, so the inaccuracy has real, if modest, blast radius —
   someone relying on "one command-buffer submission" for timing or
   synchronization assumptions would be misled. Recommend fixing the
   comment in a follow-up (trivial, no code-behavior change needed).
2. **[LOW]** `shaders/ibl/prefilter_specular.slang`'s header comment
   lists `DistributionGGX() (:140-145)` as part of what was "ported...
   as an ONLINE running sum" for the `linearRoughness > 0` branch, but
   `DistributionGGX`/its PDF-based mip-LOD-bias role is never actually
   called anywhere in the shipped kernel — it was correctly dropped
   along with the mip-chain-biasing simplification the SAME file's own
   "SIMPLIFICATIONS" section discloses two paragraphs later. Self-
   contradicted within the same file, so low-risk, but worth tightening:
   the "ported" list should not name a function that was, by the file's
   own later admission, not used.
3. **[LOW]** `bakeEnvironment()`'s `ComputePipelineCache` is created
   against a hardcoded, unparameterized path
   (`std::filesystem::temp_directory_path() / "rx_ibl_bake.cache"`,
   `bake.cpp`), shared by every caller (the bench tool, every GPU test,
   and any future Task 10 production caller) and every device/driver.
   Vulkan's own per-entry vendor/device/pipelineCacheUUID header makes
   this safe in practice (confirmed empirically — lavapipe and NVIDIA
   runs interleave against the same file with zero corruption or
   validation errors across this session's many reruns), so this is not
   a correctness bug, but a shared `/tmp` path with no per-device/
   per-caller namespacing is a mild robustness gap worth hardening
   before Task 10 introduces concurrent or multi-GPU bake callers.

No other quality issues found. Descriptor-set/pool lifetime discipline
is correct (a fresh set-1 per pass invocation, matching the documented
reason: all passes record into one command buffer before any GPU
execution begins). Barrier/layout handling for the four-graph split
(the capture-pass trick to union `SAMPLED_BIT`, the `TRANSFER_SRC_BIT`
fix, the `sourceIsCube` passthrough-via-compute redesign) is correct and
validation-clean on both drivers across every run in this session. The
hand-rolled fp16→fp32 decoder in `ibl_gpu_fixture.h` was checked by hand
(subnormal, inf/nan, and normal-range cases all trace to the correct
IEEE 754 binary32 bit pattern) and is a reasonable "no ready-made option
justifies a new dependency for one function" carve-out, disclosed as
such. All seven disclosed in-round bug fixes are real: fixes 1/2 (the
subresource-validator four-graph restructuring and the capture-pass
`SAMPLED_BIT` union) are directly visible and consistent in `bake.cpp`;
fixes 6/7 (the `UNDEFINED→GENERAL` init barrier and the maximally-
conservative `ALL_COMMANDS`/`MEMORY_READ|WRITE` sync barrier) are both
present, commented, and exercised by
`test_ibl_cube_face_convention_gpu.cpp`, which passed on both drivers
this session.

## Not independently verifiable this session

- **Windows-cross-zig/Wine 14/14 claim**: not re-run (outside this
  review's empirical-minimum list and outside the fork/GPU-isolation
  constraints governing this round); the `ci.yml` diff correctly adds
  `rx_ibl_gpu` to the Wine-job GPU-test exclusion regex, consistent with
  every other `*_gpu_tests` binary already excluded there for the same
  reason.
- **Steam Deck numbers**: correctly not published this round per RC7/RC8
  ("honest-manual until Deck hardware enters the loop"); no Deck hardware
  in this reviewer's loop either.
- **DFG LUT spot values vs. a scraped external Karis/Filament table**:
  the report deliberately did not attempt to match a scraped table
  (methodology explained and, in this reviewer's judgment, sound — see
  Port fidelity above, where the closed-form limit was independently
  re-derived and empirically confirmed rather than trusted); no
  independent external table was located/checked in this review either,
  so this specific sub-claim rests on the independently-reproduced
  closed-form derivation, not a third-party reference table.

---

# Re-check (commits `267e55a` fix + `23fb276` report addendum)

## Final verdict: **ALL FOUR ITEMS ADDRESSED.** Spec compliance flips
**FAIL → PASS.** Code quality remains **Approved**, all 3 prior findings
closed, 1 new NIT found in the addendum's own narrative (non-blocking).

**Explicit spec-flip statement:** Verdict 1 for matrix-p5t09 (as amended
by the T9 ruling) changes from **FAIL** (Task 9 review, original round)
to **PASS**. The plan/ticket's acceptance line — *"SH/irradiance VALUES
asserted against analytic ground truth (uniform white environment →
known coefficients/irradiance; directional impulse → known lobe)"* — now
has both required proofs built and independently reproduced:
`test_ibl_analytic_gpu.cpp` (uniform environment, unchanged from the
original round) and `test_ibl_directional_impulse_gpu.cpp` (new this
round). The OR-mischaracterization that caused the original FAIL is
corrected in `task-09-report.md` in place (struck through and replaced,
not silently deleted), matching this project's disclose-honestly norm.
Every matrix row is now satisfied; no open spec gap remains.

## Verification performed this round (independent, both drivers)

1. **New directional-impulse test, both drivers — CONFIRMED 7/7,
   357/357**, exactly matching the claim.
   - Lavapipe (llvmpipe/Mesa): `[doctest] test cases: 7 | 7 passed`,
     `assertions: 357 | 357 passed`, 0 failed. Full `rx_ibl_gpu_tests`
     binary, `--validate`.
   - Real NVIDIA GeForce RTX 2080, driver `580.82.07` (confirmed via
     `nvidia-smi`, default ICD, no override): identical `7/7`, `357/357`,
     0 failed. `grep "Validation Error" | grep -v "known false positive"`
     → 0 on this run's log.
   - Full serial lavapipe ctest re-run after all verification: `100%
     tests passed, 0 tests failed out of 33`.

2. **CPU-oracle independence — judged genuine, verified structurally.**
   `referenceRoughnessFilter()` in the new test file is a self-contained,
   hand-typed C++ **double-precision** re-implementation of
   `CubemapIBL::roughnessFilter()`'s estimator, in a separate translation
   unit, using its own `Double3`/`hammersley`/`hemisphereImportanceSampleDggx`
   — it does not `#include`, string-load, or otherwise derive from
   `prefilter_specular.slang`'s actual source text, and it does not call
   into `bake.cpp`'s or the shader's own code path. Its structural
   similarity to the shader (same variable roles, same tangent-basis
   construction) is expected and not a red flag: both the shader and this
   oracle are independent ports of the *same* pinned Filament formula
   (verified against the actual fetched `CubemapIBL.cpp` in the original
   round's Port Fidelity section), which is the correct methodology for
   catching an *implementation* bug in one of the two ports — exactly
   what the empirical sabotage/revert below confirms it does. This
   matches the precedent the file's own header comment cites (T4/T7/T8's
   independent-oracle pattern) and is not a novel or weaker approach.
   Genuine independence (different language, different toolchain,
   different precision, no shared code) — not merely claimed.

3. **Sabotage/revert reproduction (reviewer's own asymmetric-NoL-weighting
   sabotage, re-applied independently) — CONFIRMED the 9/39 count, both
   drivers; found one factual inaccuracy in the addendum's narrative.**
   Edited `shaders/ibl/prefilter_specular.slang`'s accumulation loop
   (`weight += noL;` → `weight += noL * noL;`), re-ran the new test case
   only (no C++ rebuild — Slang recompiles from disk at runtime):
   ```
   real NVIDIA:  test cases: 1 | 0 passed | 1 failed; assertions: 39 | 30 passed | 9 failed
   lavapipe:     test cases: 1 | 0 passed | 1 failed; assertions: 39 | 30 passed | 9 failed
   ```
   Exact match to the claimed `9/39` on both drivers, reproducible
   (re-ran twice on NVIDIA, identical). **However**, the addendum's own
   A4 section states *"ALL 9 failures at mip=3 ... offsets 0/8/16/24/35
   deg"* — this is not what either driver actually produced. The real
   breakdown, identical on both drivers: **4 failures at mip=2** (offsets
   0°/8°/16°/24°) **+ 5 failures at mip=3** (offsets 0°/8°/16°/24°/35°) =
   9. The addendum's own claim is self-contradictory on its face (only 6
   offsets are tested per mip, so "9 failures at mip=3" is arithmetically
   impossible at a single mip) — likely a miscount when writing up the
   reproduction, not a fabrication (the headline `9/39` count, the
   specific offset list, and the general "highest-roughness mips fail"
   shape are all otherwise accurate; only the "ALL... at mip=3" framing
   is wrong, mip=2's own 4 failures are real and were left out of the
   prose). Does not affect the spec-flip verdict — the test still
   discriminates the sabotage correctly, deterministically, and
   identically on both drivers, which is what matters. Flagged as a NEW
   NIT (see below), not a re-opened finding.
   Reverted (`weight += noL;`); `git diff --stat -- shaders/ibl/` empty
   — byte-identical restore confirmed; full suite back to `7/7`, `357/357`
   on real NVIDIA immediately after.

4. **Doc/comment/namespace fixes — all 3 CONFIRMED closed, correctly.**
   - `bake.h`: the stale "SINGLE render-graph... one command-buffer
     submission" claim and the stale "`sourceIsCube` copied via
     `vkCmdCopyImage`" claim are both gone; the comment now correctly
     describes the four-graph design and the compute-passthrough
     `sourceIsCube` path, with a pointer to `bake.cpp`'s own "Design
     decision" comment. Grepped the file directly for both stale phrases
     — zero hits.
   - `prefilter_specular.slang`: the header no longer lists
     `DistributionGGX()` as ported; it now explicitly states the function
     is NOT ported and explains why (only ever needed for the dropped
     mip-LOD-bias math) — no longer self-contradicts the SIMPLIFICATIONS
     section.
   - Cache path: `bakeEnvironment()` gained a `cacheNamespace` parameter
     (default `"default"`), used to build
     `<temp>/rx_ibl/<cacheNamespace>.pipeline_cache`. Confirmed every
     call site in the module (5 test files, the bench tool) now passes a
     distinct, purpose-specific namespace (grepped each caller
     individually in the diff). Confirmed live in a real run: this
     session's lavapipe log shows `saved 32 bytes ... to
     '/tmp/rx_ibl/test_hdr_fixture.pipeline_cache'` — the namespaced path
     is genuinely in effect, not just documented.

5. **Commit hygiene — CONFIRMED.** Two commits (`267e55a` fix,
   `23fb276` chore), both authored/committed by `Yousef Wadi
   <ywadi85@gmail.com>`, correctly pathspec-split (code+tests in the fix
   commit, report-only in the chore commit). No AI attribution anywhere
   (`git diff 3299993..23fb276 | grep -iE "claude|anthropic|co-authored"`
   → no hits once the benign "generated by this task" HDR-fixture-provenance
   line is excluded, same as the original round). `git status --porcelain`
   clean except the same pre-existing, untouched `progress.md`
   modification. 4 commits ahead of `origin/main` now, nothing pushed.

## New finding

6. **[NIT]** `task-09-report.md`'s Addendum §A4 claims *"ALL 9 failures
   at mip=3"* for the sabotage reproduction; the actual, reproducible
   breakdown (confirmed twice on real NVIDIA and once on lavapipe this
   session) is 4 failures at mip=2 + 5 at mip=3. The claim is internally
   impossible as written (max 6 offsets exist per mip) and should be
   corrected to avoid future confusion, but it does not affect the
   test's validity, the 9/39 headline count, or the spec-flip verdict —
   the test genuinely and deterministically discriminates the sabotage
   on both drivers regardless of which two mips it lands on.

No other issues found. All four items from the coordinator's scoped
re-check are addressed as claimed, with the one narrative-accuracy nit
above.
