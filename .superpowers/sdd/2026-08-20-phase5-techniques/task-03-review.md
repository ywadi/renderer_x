# Review — Task 3 (#39): HDR scene-color single source + FG6 no-MSAA ruling

Reviewer round. Commit under review: `e341433` (single commit, parent
`9f7dfb2`). Order of authority followed: `rulings-2026-08-20.md` ("T3
(#39)") > plan > `matrix-p5t03-hdr-scene-color.md` (rows as amended by the
ruling) > ticket #39. Independent of the implementer; every finding below
is from direct code reading, an independent tree-wide grep, reproduced
build/test evidence on both drivers, an independent mantissa-arithmetic
derivation, and a self-applied mutation test — not from re-trusting the
task-03-report.md narrative.

## Verdict 1 — Spec compliance: **PASS**

Both ruled decisions (B10G11R11 default + A16B16G16R16F escape hatch; FG6
no-MSAA/TAA-first as a documented rejection) are implemented exactly as
ruled, and every matrix row (as amended) is satisfied by the actual code:

- **Single-sourced format.** `rx::graph::kHdrFormat =
  VK_FORMAT_B10G11R11_UFLOAT_PACK32` and `kHdrFormatHighPrecision =
  VK_FORMAT_R16G16B16A16_SFLOAT` live in one new header,
  `src/rx_graph/include/rx_graph/scene_color.h`. All four prior
  independent `constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;`
  copies (05_multipass, 07_stress, 08_gltf_viewer, 09_scene) are removed;
  every call site (`.addColorOutput`, `pColorAttachmentFormats`,
  `PassSignature::colorFormats[0]`) now reads `rx::graph::kHdrFormat`.
  Independently grepped the whole tree for `kHdrFormat\s*=` and for both
  `R16G16B16A16_SFLOAT`/`B10G11R11` outside `scene_color.h` — the only
  remaining hardcodes are two **pre-existing**, unrelated device-free
  fixtures (`src/rx_graph/tests/test_barriers.cpp:49`,
  `test_compile.cpp:28`, both from ticket #38/`b73c563`, predating this
  task, confirmed via `git show 9f7dfb2:...`) using RGBA16F as an
  arbitrary generic `AttachmentDesc` color format for barrier/compile
  plumbing tests unrelated to the scene-color seam — not a completeness
  gap in this ticket's migration claim.
- **Format ruling adopted verbatim**, matching `rulings-2026-08-20.md`'s
  "T3 (#39)" line exactly (B10G11R11 default, A16B16G16R16F escape
  hatch, both documented with rationale and Filament precedent in
  `scene_color.h:10-98`).
- **FG6 ruled no-MSAA/TAA-first, recorded as a documented rejection, not
  an implementation.** Verified directly against current code, not just
  against the header's own citations: `Texture2D::create()` and
  `createForPresuppliedMips()` both still hardcode `imageInfo.samples =
  VK_SAMPLE_COUNT_1_BIT` at `src/rx_rhi_vk/src/texture.cpp:133,213`
  exactly as cited; `texture.h` has no `samples` parameter anywhere;
  whole-tree grep for `VK_SAMPLE_COUNT_[248]` outside a hardcoded
  `VK_SAMPLE_COUNT_1_BIT` assignment returns nothing (no `samples > 1`
  usage snuck in anywhere, including the new test file, which explicitly
  sets `VK_SAMPLE_COUNT_1_BIT`). Whole-tree grep for
  `resolveMode|resolveImageView|pResolveAttachment|VK_RESOLVE_MODE`
  returns only `Executor::resolveImageView(std::string_view name)` /
  `PassContext::imageView()` — a pre-existing, unrelated **resource-name
  lookup** function (resolves a graph resource name string to its
  `VkImageView`), not MSAA resolve-attachment wiring; zero real
  resolve-target plumbing exists anywhere, confirming both structural
  gaps as stated.
- **Consumer table (row 6)** and **tonemap-hook documentation (row 7)**
  are both present and accurate: both `tonemap.frag.slang` copies
  (multipass/stress) are untouched by this commit (confirmed via `git
  diff 9f7dfb2 e341433 -- shaders/` — empty) and independently reconfirmed
  byte-identical to each other after comment-stripping.
- **Row 9 ownership** (migrate all four call sites in the introducing
  commit) is satisfied — grep-confirmed zero sample-local `kHdrFormat`
  declarations remain.
- **Row 10 storage-image caveat** is logged, not silently assumed —
  confirmed both formats' `STORAGE_IMAGE_BIT` empirically true on this
  session's own re-run (see Verdict 2 below), with the header's own
  caveat correctly scoping this as informational, re-query-required by
  future compute consumers.
- **Row 8 (existing pixel gates)** — the report's "byte-identical, no
  regeneration needed" claim independently re-verified (see below); no
  reference PNGs were touched by this commit (`git diff 9f7dfb2 e341433
  --stat -- '*.png'` empty; `git log` on the two references' directories
  shows no entry newer than pre-existing fix commits).

No matrix row was left unresolved, and none of the two "needs-coordinator-
decision" rows (3 and 4) were implemented against the matrix's own
tentative recommendation text — both were implemented against the actual
binding ruling text, correctly treating "rulings > matrix" per this
ticket's own stated order of authority.

## Verdict 2 — Code quality: **Approved**

No blocking findings. The header (`scene_color.h`) is well-organized,
heavily and accurately cross-cited, and the GPU test file follows this
codebase's established fixture idioms (independent per-file
`makeFixture`/`OffscreenImage`/fill-pipeline copies, matching
`test_execute_gpu.cpp`/`test_compute_gpu.cpp` precedent, not shared
prematurely).

**Minor (non-blocking):**
- `scene_color.h:80,214` reference `SceneColorGpu::EscapeHatchPreserves...`
  and `SceneColorGpu::FormatSupportsColorAttachmentAndLinearSampling` as
  if these were real C++ symbol names; doctest `TEST_CASE` here uses
  plain string descriptions, not named test symbols, so these two
  identifiers don't exist anywhere and can't be grepped for verbatim
  (confirmed: zero hits for either string in `test_scene_color_gpu.cpp`).
  Cosmetic only — the actual test-case description strings are quoted
  correctly and unambiguously elsewhere in the same comments.

**Nit:** `renderAndReadbackTexel()`'s `std::array<uint8_t, 8> raw{}` is
sized for the escape hatch's 8-byte texel and only the first `texelBytes`
are ever populated/read by callers that pass 4 — correct and safe (no
OOB), just worth a caller noting the array is oversized for the 4-byte
case; not worth a code change.

## GPU test honesty — independently re-derived and re-run

**Mantissa arithmetic, derived independently before looking at the test's
own comment:** for input 691/160 = 4.31875, mantissa fraction (relative to
the implicit leading 1 at exponent 2) = 51/640. In R/G's 6-bit grid units:
51/640×64 = 5.1; in B's 5-bit grid units: 51/640×32 = 2.55. Under
truncation (confirmed empirically to be this hardware/driver's actual
rounding behavior, see below): R/G mantissa → 5/64 → decoded 4.3125; B
mantissa → 2/32 → decoded 4.25. errorR = 0.00625, errorB = 0.06875,
errorB > errorR strictly — holds under EITHER truncation or
round-to-nearest (round-to-nearest would give B → 4.375, errorB=0.05625,
still > errorR), so the test's claimed rounding-convention-independence is
correct, not just asserted.

**Re-run myself, both drivers, `rx_graph_gpu_tests -tc="SceneColorGpu*"`:**

| Driver | R/G decoded | B decoded | errorR | errorB | UNORM8 mutant (R,G,B) | Escape hatch (R, G=B, A) |
|---|---|---|---|---|---|---|
| lavapipe (`lvp_icd.json`) | 4.3125 | 4.25 | 0.0062499 | 0.0687499 | 255,255,255 | -2.5, 4.31641, 0.75 |
| real NVIDIA RTX 2080 (`nvidia_icd.json`, default ICD) | 4.3125 | 4.25 | 0.0062499 | 0.0687499 | 255,255,255 | -2.5, 4.31641, 0.75 |

Identical on both drivers, matching the report's claimed values exactly.
`hasValidationErrors()` false throughout both runs; grepped raw NVIDIA
output for `[vulkan validation]` lines not marked
`known false positive` — zero hits.

**Escape-hatch decode cross-checked independently:** fp16(4.31875) under
truncation (10-bit mantissa, exponent 2, mantissa units 0.0796875×1024 =
81.6 → floor 81 → 81/1024) decodes to 4.31640625 ≈ 4.31641 — matches the
observed `decoded.g == decoded.b == 4.31641` on both drivers, confirming
this driver stack truncates (rounds toward zero) rather than
round-to-nearest on float→packed-float attachment writes, consistently
across both the B10G11R11 and RGBA16F cases and across both drivers — an
interesting empirical fact the report did not call out explicitly, but
one that does not affect the test's correctness (the test's own
discrimination claim is proven to hold under either convention, and it
does).

**Self-applied mutation test (temporary edit, restored byte-identically):**
Changed `scene_color.h`'s `kHdrFormat` to `VK_FORMAT_R8G8B8A8_UNORM`,
rebuilt both `rx_graph_tests` and `rx_graph_gpu_tests`, and confirmed:
- The device-free pinning test (`test_scene_color.cpp`) fails loudly and
  exactly: `CHECK( kHdrFormat == VK_FORMAT_B10G11R11_UFLOAT_PACK32 )` →
  `37 == 122` (NOT correct).
- The GPU value-survival test fails with garbage, not silently: since the
  fill+readback now actually renders/packs UNORM8 bytes but the test's
  decoder is still the packed-float `glm::unpackF2x11_1x10`, the
  reinterpreted bit pattern decodes to nonsense (`decodedR=130048,
  decodedB=-1, errorB=5.31875` vs the expected ~4.3), and 5 of 8
  assertions in that test case fail explicitly.
Restored `scene_color.h` to its original content
(`md5sum` before/after identical: `d555aa5de332ee41ebb12e05775cefb7`),
rebuilt again, and reconfirmed `rx_graph_tests`/`rx_graph_gpu_tests` both
green on lavapipe post-restore. This confirms the tests do not pass by
construction — a broken production constant is caught immediately and
specifically, at both the device-free and GPU layers.

## D17 byte-identical claim — independently re-run, lavapipe

Ran `sample_08_gltf_viewer --validate` and `sample_09_scene --validate`
myself (not via ctest's summary alone — direct stdout inspection):

- `sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536
  (0.0000%) pass=true`
- `sample_08_gltf_viewer: D17 loaded_scene gate: failingPixels=0/65536
  (0.0000%) pass=true`
- `sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%)
  pass=true`

All three exactly 0/65536, matching the report's claim and confirming no
reference PNGs needed regeneration for this format switch on these
specific committed frames. Confirmed via `git log`/`git diff --stat` that
none of the three reference PNGs were touched by this commit — the
byte-identical claim is genuine, not achieved by quietly re-baking the
references.

**C1 discrimination re-proof, re-run myself (still fires, unaffected):**
lavapipe `240/65536 (0.3662%)`; real NVIDIA `241/65536 (0.3677%)` —
both match the report exactly and both comfortably clear the
`kMinDiscriminatingPixels = 100` floor, confirming the shadow-discrimination
gate was not silently defeated by this format change.

**Real-driver informational divergence, re-confirmed (pre-existing,
unrelated to this task):** `loaded_scene 425/65536 (0.6485%) pass=false
[non-lavapipe driver -- informational only, not enforced]`, `grid_scene
556/65536 (0.8484%) pass=false [non-lavapipe driver -- informational only,
not enforced]` — both `[non-lavapipe driver]`-labeled, both overall
`headless gate PASSED`. Matches the report's cited numbers exactly.

## doctest workaround — verified test-code-only

`git show e341433 --stat` restricted to non-test/non-CMakeLists/non-
scene_color.h/non-sample paths is empty — nothing outside the listed test
and sample files changed. The `std::string(label)` wrap for
doctest 2.5.3's `INFO`/`MESSAGE` stringification bug appears only in
`test_scene_color_gpu.cpp:670-676`; no vendored `doctest.h` or any
production header/source was touched. Isolation claim holds.

## Empirical verification performed (driver-labeled)

- Full serial `ctest -j1`, **lavapipe** (`lvp_icd.json`, `xvfb-run`, NICEd,
  foreground): **29/29 passed**, 80.41s.
- Full serial `ctest -j1`, **real NVIDIA RTX 2080** (default ICD via
  `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json`, on-desktop
  `DISPLAY=:1`, NICEd, foreground, serialized — no other GPU work
  concurrent): **29/29 passed**, 146.53s.
- `rx_graph_gpu_tests`'s 4 `SceneColorGpu` cases individually re-run with
  `-s` (full assertion trace) on both drivers — see table above.
- `rx_graph_gpu_tests` alone also re-run standalone (ctest wrapper) on
  real NVIDIA — passed, 6.84s; zero unfiltered `[vulkan validation]`
  lines.
- D17 gates (08_gltf_viewer, 09_scene) re-run standalone on lavapipe with
  raw stdout inspected directly.
- Mutation test (kHdrFormat → UNORM8, rebuild, observe exact failures,
  byte-identical restore, rebuild) — see above.
- Commit hygiene: single commit (`e341433`, parent `9f7dfb2`); `git show
  --stat` confirms exactly the 8 files the report's "What shipped"
  section names, nothing else; author `Yousef Wadi
  <ywadi85@gmail.com>` matches local `git config user.name`/`user.email`
  exactly; commit message and every touched file grepped for
  `claude|anthropic|co-authored|generated by|ai assistant` — zero hits
  (one incidental match on the literal string "CLAUDE.md", the repo's own
  policy filename, not AI attribution); `progress.md` untouched by the
  commit (confirmed in `git show --stat`) and its pre-existing
  working-tree modification was never touched by this review; branch is
  ahead of `origin/main` by exactly this 1 commit, nothing pushed.
- Windows-cross-zig build (13/13, per the report) was **not**
  independently re-run — outside this review's stated empirical minimum
  and this commit touches no Windows-relevant code path (device-free
  tests + samples' own `main.cpp` edits are cross-platform-generic); flagged
  here as not independently verified rather than silently assumed true.

## Not independently verifiable this round

- The exact Vulkan mandatory-format-support table row for
  `VK_FORMAT_B10G11R11_UFLOAT_PACK32`'s `STORAGE_IMAGE_BIT` guarantee
  remains routed through empirical query only (both this task's report
  and this review's own re-run confirm it TRUE on lavapipe + this one
  NVIDIA GPU) — never claimed as a portable spec guarantee by either the
  header or this review; Task 2/#38's future compute consumers must
  still re-query on their own target hardware (Deck/RADV) per
  `scene_color.h`'s own caveat, unchanged by this review.
- Windows-cross-zig 13/13 claim (see above) — not independently re-run.
- The float32→packed-format truncation-vs-round-to-nearest hardware
  behavior observed identically on both drivers this session was not
  cross-checked against a third driver (e.g. RADV) — noted as an
  interesting empirical fact, not asserted as universal.

## Restoration

The only working-tree edit made during this review (`scene_color.h`'s
`kHdrFormat` mutation for the discrimination re-proof) was reverted and
confirmed byte-identical via `md5sum` before/after
(`d555aa5de332ee41ebb12e05775cefb7`), then rebuilt so the build directory
matches the restored source. `git status --porcelain` at review end shows
only the pre-existing `.superpowers/sdd/2026-08-20-phase5-techniques/
progress.md` modification, unchanged from review start — left alone per
instruction.
