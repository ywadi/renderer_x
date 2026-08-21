# Task 3 report — HDR scene-color infrastructure + MSAA policy (issue #39)

Implementer round. Base: main `9f7dfb2`. Order of authority followed:
rulings (`rulings-2026-08-20.md`, "T3 (#39)") > plan (Task 3) > gate matrix
(`matrix-p5t03-hdr-scene-color.md`) > ticket (#39).

## Status: COMPLETE

All matrix rows (as ruled) satisfied. Both presets green. Real-driver
(NVIDIA RTX 2080) run clean, zero unfiltered validation errors. No
production defects found; one real, empirically-confirmed doctest 2.5.3
limitation found and worked around in test code only (see "Deviations").

## What shipped

**Single-sourced HDR scene-color convention (`src/rx_graph/include/
rx_graph/scene_color.h`, new):**
- `kHdrFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32` — the ruled
  process-wide default, replacing four independent
  `constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;` copies
  (`samples/05_multipass`, `07_stress`, `08_gltf_viewer`, `09_scene` — all
  four migrated in this commit, zero sample-local declarations remain,
  grep-verified).
- `kHdrFormatHighPrecision = VK_FORMAT_R16G16B16A16_SFLOAT` — the ruled,
  documented escape hatch (signed values / real alpha / symmetric
  higher-per-channel precision), exercised by its own GPU test (not merely
  declared).
- `kSceneColorResourceName = "hdr"` — the canonical resource name (matches
  every sample's existing copy-paste convention; not force-migrated at
  call sites, since the ruling's scope is the *format* single-sourcing,
  not a resource-name rename).
- A committed consumer table (matrix row 6's own acceptance criterion)
  mapping every charter frame-pipeline-target stage after "opaque
  lighting" to the task that attaches to it (mip chain → Task 22, SSR →
  Task 26, glass/transmission → Tasks 23/24, bloom → Task 31, tonemap →
  Task 32, TAA → Task 33).
- The FG6 MSAA policy ruling recorded in full: no-MSAA/TAA-first, with
  rationale and the two concrete, re-verified structural gaps (samples
  axis dead-ends at `Texture2D::create()`, still hardcoded to
  `VK_SAMPLE_COUNT_1_BIT` at `texture.cpp:133,213`; zero resolve-target
  wiring anywhere in `AttachmentDesc`/`PhysicalResource`/`executor.cpp`) —
  documentation only, no MSAA implementation, per the ruling.
- A forward-looking storage-image caveat note (matrix row 10) for Task
  2's future compute consumers (SSR/volumetrics), citing this task's own
  empirical `STORAGE_IMAGE_BIT` query results.

**Tests added (all passing, both presets, both drivers):**
- `src/rx_graph/tests/test_scene_color.cpp` (new, device-free, joins
  `rx_graph_tests`): pins `kHdrFormat`/`kHdrFormatHighPrecision` to their
  ruled `VkFormat` values, asserts they're distinct, pins the canonical
  resource name.
- `src/rx_graph/tests/test_scene_color_gpu.cpp` (new, joins
  `rx_graph_gpu_tests`), this ticket's own primary gate — 4 TEST_CASEs:
  1. **Value survival + honest precision proof**: a real fullscreen-triangle
     draw writes 4.31875 to all three channels of a real
     `kHdrFormat`-formatted attachment; the raw packed texel is copied
     back and decoded with `glm::unpackF2x11_1x10` (GLM's own reference
     implementation of this exact bit layout — reused, not
     reimplemented). Proves the value survives (>4.0, not clamped) AND
     that blue's narrower 5-bit mantissa produces a strictly larger
     rounding error than red/green's 6-bit mantissa, for an input value
     deliberately chosen so that inequality holds under EITHER
     round-to-nearest OR truncating hardware rounding conventions (proven
     algebraically before writing the test, then confirmed empirically:
     both lavapipe and the real NVIDIA driver decode identically —
     R=G=4.3125, B=4.25, errorR=0.00625, errorB=0.06875).
  2. **Discrimination mutant**: the identical value/mechanism against a
     deliberately wrong `VK_FORMAT_R8G8B8A8_UNORM` target clamps to byte
     255 (~1.0) — proves the test actually discriminates HDR survival
     rather than passing by construction.
  3. **Escape hatch exercised**: `kHdrFormatHighPrecision` preserves a
     genuinely negative value (`-2.5`, something `kHdrFormat` cannot
     represent at any precision, not just imprecisely) and a real alpha
     value, and decodes G/B (both driven from the same input `kHdrFormat`
     test used) with IDENTICAL, symmetric precision — the direct contrast
     to `kHdrFormat`'s proven blue deficit.
  4. **Empirical format-support query**: `vkGetPhysicalDeviceFormatProperties`
     on the real device (never a memorized spec citation, per matrix row
     3/10's own verification-health note) confirms
     `COLOR_ATTACHMENT_BIT`/`SAMPLED_IMAGE_FILTER_LINEAR_BIT` for both
     formats; `COLOR_ATTACHMENT_BLEND_BIT`/`STORAGE_IMAGE_BIT` logged
     (informational, per row 10) — both true for both formats on both
     lavapipe and the real NVIDIA driver.

**Samples migrated (all four, in this commit — matrix row 9's ownership
call: whichever ticket introduces the shared constant migrates every call
site):**
- `samples/05_multipass/main.cpp`, `samples/07_stress/main.cpp`,
  `samples/08_gltf_viewer/main.cpp`, `samples/09_scene/main.cpp`: own
  `kHdrFormat` declarations removed; every use site (pass declarations,
  `PipelineRenderingCreateInfo::pColorAttachmentFormats`,
  `PassSignature::colorFormats[0]`) now reads `rx::graph::kHdrFormat`.
  One stale doc comment in `05_multipass/main.cpp` (still naming
  `R16G16B16A16_SFLOAT` in its graph-shape header) corrected.
- `shaders/multipass/tonemap.{vert,frag}.slang` and
  `shaders/stress/tonemap.{vert,frag}.slang` (the matrix's own flagged
  "third copy the ticket's file list omits") — **untouched, deliberately**:
  both already sample scene color via a bindless texture index with no
  format assumption baked into the shader; confirmed still byte-identical
  in logic to each other after this task (comment-stripped diff empty).

## Per-row proof (matrix)

| # | Criterion | Disposition | Evidence |
|---|---|---|---|
| 1 | HDR value survives by VALUE, with a discrimination mutant | Delivered | `test_scene_color_gpu.cpp` cases 1+2; real raw-texel readback, both drivers |
| 2 | One engine-owned constant, pinned by a test, precision documented | Delivered | `scene_color.h` + `test_scene_color.cpp`; doc comment states bit layout/precision facts |
| 3 | Format ruling: B10G11R11 | Ruling adopted verbatim | `kHdrFormat`; rationale + Filament precedent + Deck-bandwidth argument recorded in `scene_color.h` |
| 4 | FG6 MSAA ruling: no-MSAA/TAA-first | Ruling adopted verbatim | Documented rejection in `scene_color.h`, with rationale |
| 5 | Graph's single-sample assumption documented (since row 4 rules MSAA out) | Delivered | `scene_color.h`'s FG6 section cites `texture.cpp:133,213` (re-verified current) and the zero-resolve-wiring grep |
| 6 | Scene-color seam documented, consumer table | Delivered | `scene_color.h`'s consumer table, task-cited |
| 7 | Tonemap hook documented, not redesigned | Confirmed unchanged | No shader edits; both tonemap copies still logic-identical (diff empty) |
| 8 | Existing pixel gates regenerated w/ provenance + discrimination intact, zero validation errors both drivers | **No regeneration needed — verified, not assumed** | See "Deviations" below |
| 9 | `kHdrFormat` duplication — ownership | This ticket introduces AND migrates all 4 sites | Grep: zero sample-local `kHdrFormat` declarations remain |
| 10 | Storage-image caveat, forward-looking | Logged | Both formats' `STORAGE_IMAGE_BIT` empirically TRUE on both lavapipe and NVIDIA RTX 2080 (logged in test output, noted in `scene_color.h` as still requiring a future task's own re-query, never assumed transitively) |

## Deviations from the matrix's stated expectation

**Row 8 — no reference-PNG regeneration was needed, confirmed empirically
rather than assumed either way.** The matrix's own row 8 correctly
predicted two possible outcomes ("byte-identical, no visual change
expected" for a pure refactor vs. "new baseline, bounded precision-shift
documented" for an actual format switch) and flagged this as a real format
switch, not a pure refactor. I ran both committed-reference gates
BEFORE assuming either outcome:
- `sample_08_gltf_viewer --validate` (lavapipe): `loading_state`
  failingPixels=0/65536, `loaded_scene` failingPixels=0/65536 — both
  0.0000%, gate PASSED.
- `sample_09_scene --validate` (lavapipe): `grid_scene`
  failingPixels=0/65536 — 0.0000%, gate PASSED.

Both are BYTE-IDENTICAL to their pre-existing committed references at the
D17 tolerance (±4/255 per channel, <0.5% failing-pixel budget) — the
DamagedHelmet/grid-scene content's actual post-tonemap radiance range does
not shift enough for B10G11R11's coarser precision to move any pixel past
one 8-bit LSB after Reinhard tonemapping, for these specific committed
frames. `tools/regen_references.sh` was therefore NOT run and no PNGs were
touched — regenerating with no actual delta would be pure churn, and (per
that script's own header comment) would silently narrow the gate's future
discriminating power for no reason. The C1 shadow-discrimination re-proof
(`sample_09_scene`) still fails-on-purpose: 240/65536 (0.3662%) differing
pixels on lavapipe with shadows forced off (≥ the 100-pixel floor),
241/65536 (0.3677%) on the real NVIDIA driver — unaffected by this task,
confirmed re-run.

On the real NVIDIA driver specifically, the D17 gates report
`pass=false` (`loaded_scene` 425/65536 = 0.6485%; `grid_scene` 556/65536 =
0.8484%) — this is PRE-EXISTING, EXPECTED, documented behavior
(`samples/09_scene/main.cpp`'s own D17 gate mechanism enforces the
lavapipe-vs-reference comparison as a hard PASS/FAIL only on lavapipe;
every other driver's divergence is logged `[non-lavapipe driver --
informational only, not enforced]`), unrelated to this task's format
change — the overall `headless gate PASSED` on the real driver confirms
the actually-enforced check succeeded.

**A real, empirically-confirmed doctest 2.5.3 limitation, worked around in
test code (test-only, no production impact).** `MESSAGE`/`INFO`'s variadic
macro stringifies a NAMED `const char*` LVALUE incorrectly (prints "1"
instead of the string content) — a string LITERAL argument is unaffected,
which is why this was not already a known issue in this codebase's other
test files. Reproduced in isolation with a 10-line standalone repro
against this project's own vendored doctest.h before concluding it wasn't
a mistake in my own code. Fixed in `test_scene_color_gpu.cpp` by wrapping
the format label in `std::string(...)` before passing it to `INFO`/
`MESSAGE`, confirmed correct on rebuild (both drivers).

## Both-preset / both-driver verification (command tails)

Lavapipe, full suite, linux-native:
```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  81.17 sec
```

Windows-cross-zig, full build + Wine ctest (T2's own exclusion pattern):
```
ninja                                    # 25/25, zero errors
xvfb-run -a ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample' -j1
...
100% tests passed, 0 tests failed out of 13
Total Test time (real) = 106.54 sec
```

Real driver (NVIDIA GeForce RTX 2080, default ICD —
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json`, `DISPLAY=:1`,
on-desktop, serialized/NICEd per the standing owner rule, full suite):
```
100% tests passed, 0 tests failed out of 29
Total Test time (real) = 145.40 sec
```
`rx_graph_gpu_tests`' 4 new `SceneColorGpu` cases individually re-run with
`--validate` on both lavapipe and the real driver, output inspected
directly (not just the ctest pass/fail summary) — see "What shipped"
above for the exact decoded values, identical on both drivers. Every
"[vulkan validation]" line observed across the whole session matches this
codebase's own pre-existing, documented false-positive filter set
(`context.cpp`'s `debugCallback()`); zero unfiltered validation errors on
any run (`hasValidationErrors()` false throughout, asserted by every new
GPU test case).

## Self-review

- **TDD discipline**: `scene_color.h`'s constants were written first, then
  `test_scene_color.cpp`/`test_scene_color_gpu.cpp` written and built
  against them (compile failures until the header existed); the precision-
  asymmetry test's expected values were derived algebraically BEFORE
  running, then the real run's output was compared against that derivation
  (matched exactly) rather than the test being reverse-fitted to whatever
  the GPU happened to produce.
- **No deferred fixes**: the doctest stringification issue is fixed in the
  one file it affected, not routed around with a TODO.
- **Revert-discrimination**: the discrimination-mutant test case (UNORM8
  target) IS the required revert-discrimination proof for row 1 (matrix's
  own explicit ask) — confirmed by direct inspection of its output (byte
  255 on all three channels) on both drivers, not just "0 failed".
- **No AI attribution**: none added anywhere (commit messages, code
  comments, this report).
- **Commit scope**: pathspec-scoped to exactly the files listed under
  "What shipped" above; `.superpowers/sdd/2026-08-20-phase5-techniques/
  progress.md` is concurrently maintained by the coordinator (per `git
  status` at session start/throughout) and is deliberately excluded from
  this commit.
- **Scope discipline**: no MSAA implementation, no resolve-attachment
  work, no `Texture2D::create()` changes — the ruling is explicit that
  this task WRITES the FG6 policy down, it does not implement MSAA;
  confirmed the two structural gaps (samples axis, resolve wiring) are
  unchanged from the matrix's own citations before documenting them as
  current fact.
- **Concerns for the coordinator**: (1) the storage-image caveat (row 10)
  found BOTH formats' `STORAGE_IMAGE_BIT` true on this session's two
  drivers (lavapipe + NVIDIA RTX 2080) — a data point in kHdrFormat's
  favor, but per the matrix's own discipline this does not retire the risk
  for OTHER hardware (RADV/Deck); Task 2's future compute consumers
  (SSR/volumetrics) must re-query on their own target hardware before
  assuming it, as `scene_color.h`'s own comment states; (2) the D17 gate's
  real-driver "informational only" divergence (0.65%/0.85%) is pre-existing
  Phase 4 behavior, not introduced by this task, but is worth the
  coordinator's awareness if a future task tightens that gate's
  enforcement scope.
