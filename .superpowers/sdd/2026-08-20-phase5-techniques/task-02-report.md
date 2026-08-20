# Task 2 report — Compute pipeline capability (issue #38)

Implementer round. Base: main `1c01881`. Order of authority followed:
rulings (`rulings-2026-08-20.md`) > plan (Task 2) > gate matrix
(`matrix-p5t02-compute-pipeline.md`) > ticket (#38).

## Status: COMPLETE

All matrix rows (as amended by RC2 + the per-ticket ruling) satisfied.
Both presets green. Real-driver (NVIDIA RTX 2080) run clean. One
significant, unanticipated defect found and fixed (see "Deviations").

## What shipped

**rx_graph resource-model extension (RC2's widened scope, landed once, as
directed):**
- `resources.h`: `Subresource` (mip/layer range, sentinel-resolved by
  `compile()`), `ImageDesc` (storage-image declared shape — format/size/
  mipLevels/arrayLayers/cube), `PhysicalResource::mipLevels/arrayLayers/cube`.
- `pass.h`/`render_graph.cpp`: `Pass::addStorageImageOutput()`/
  `addStorageImageInput()` (mirrors `addStorageBufferOutput`/`Input`'s
  shape exactly, per the per-ticket ruling); `addTextureInput()` grew an
  optional `Subresource` parameter (source-compatible, no existing call
  site touched). `AccessKind::StorageImageOutput/Input` resolve to
  `VK_IMAGE_LAYOUT_GENERAL`, compute/graphics stage split mirroring
  `StorageBufferOutput/Input`. Cube validation (`arrayLayers` must be a
  positive multiple of 6) at the establishing declaration. A new
  compile()-time validator rejects two declared subresource ranges against
  the same resource that overlap without being identical — real, disjoint,
  or identical ranges are supported; true partial-overlap tracking is an
  explicit, stated scope boundary (no named Phase-5 consumer needs it; a
  future one that does gets a clear, loud error to extend from, not silent
  wrong behavior).
- `barriers.h`/`barriers.cpp`: `ImageBarrier` carries a resolved
  `Subresource`; `buildBarriers()`'s per-resource state is now keyed by
  `(physicalIndex, Subresource)`, not `physicalIndex` alone — the actual
  per-mip/per-layer independence RC2 asks for. Every pre-existing resource
  (single-mip/single-layer) resolves to exactly one key, byte-identical to
  the old behavior.
- `executor.cpp`/`transient_pool.h/.cpp`: `PooledStorageImage` +
  `TransientPool::acquireStorageImage()`; `Executor::realize()` routes a
  `VK_IMAGE_USAGE_STORAGE_BIT` resource to the new pool; `applyBarriers()`
  threads the resolved `Subresource` into the real
  `VkImageMemoryBarrier2::subresourceRange` (replacing the hardcoded
  `VK_REMAINING_MIP_LEVELS`/`VK_REMAINING_ARRAY_LAYERS` RC2 names) for
  every image barrier — a real, not cosmetic, change: for a 1-mip/1-layer
  resource the resolved range is `{0,1,0,1}`, identical in effect to the
  old `VK_REMAINING_*`; for a storage image it is the genuine narrower
  range. New `PassContext::storageImageView(name)` resolver, backed by a
  per-pass (not global) lookup rebuilt each pass from that pass's own
  declarations — required because two different passes may declare the
  same resource name at two different subresources.

**rx_rhi_vk: new RHI primitives (per-ticket ruling: parallel facility,
NOT inside MaterialSystem):**
- `storage_image.h/.cpp` — `rx::rhi::StorageImage`: a VMA-backed image
  supporting arbitrary mip/array/cube shape (Texture2D itself is
  deliberately untouched — "always 2D, always one layer" stays true for
  every existing caller; this is a new, purpose-built sibling, not an
  invasive change to a heavily-depended-on class). Owns a full view plus
  an on-demand, cached subresource-view store (`viewForSubresource()`);
  unconditionally carries `VK_IMAGE_USAGE_STORAGE_BIT` and
  `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` (the latter because `ImageDesc` has no
  usage-request field the way `BufferDesc` does — without it no caller
  could ever read a compute-written image back at all; mirrors
  `Texture2D::create()`'s own unconditional `TRANSFER_DST_BIT`
  precedent).
- `compute_pipeline.h/.cpp` — `rx::rhi::ComputePipelineCache`: builds/
  caches `VkPipeline`s from a compiled+reflected compute module via the
  *existing* `PipelineLayoutBuilder::build()` (row 3), own
  `VkPipelineCache` using MaterialSystem's exact load/persist pattern
  (row 9, corrected), cache key = FNV-1a over the SPIR-V words alone (row
  8/Open Question 4 — no `PassSignature`, structurally simpler than
  `PipelineKey`).

**rx_shader: a real, load-bearing fix the matrix's "zero changes needed"
claim did not anticipate** — see "Deviations."

**Tests added (all passing, both presets, both drivers):**
- `rx_graph/tests/test_compile.cpp`: +11 device-free cases (storage-image
  establishment/shape, cube validation, subresource sentinel resolution,
  narrowing, overlap-throw, disjoint-accept, identical-accept,
  addTextureInput subresource).
- `rx_graph/tests/test_barriers.cpp`: +2 device-free cases proving
  per-(resource,subresource) barrier-state independence (disjoint mips
  each get their own fresh first-use barrier; a WAW on one mip does not
  leak into an unrelated mip's state).
- `rx_graph/tests/test_compute_gpu.cpp` (**new file**, the ticket's own
  primary gate): 2 GPU TEST_CASEs — (1) real compute PSO writes a storage
  buffer AND a storage image, bindless set 0 read *live* from compute
  (not just layout-compatible), a real downstream graph pass reads both,
  exact value readback (buffer: `idx*7+3+multiplier`; image:
  `(x,y,multiplier,1)`), zero validation errors; (2) two compute passes
  write disjoint faces of a cube storage image (RC2's cube/array +
  per-layer proof end to end), independent per-face exact readback.
- `rx_rhi_vk/tests/storage_image_test.cpp` (**new file**): plain/cube/
  multi-mip-array creation, `viewForSubresource()` whole-resource
  shortcut + distinct-and-cached per-range views, move semantics.
- `rx_rhi_vk/tests/compute_pipeline_test.cpp` (**new file**): PSO
  creation from a real reflected module, content-hash cache-hit reuse,
  bindless set-0 substitution.
- `rx_shader/tests/reflection_test.cpp`: +1 case (matrix row 2's own
  literal ask — compute module with `RWStructuredBuffer<uint>` +
  `RWTexture2D<float4>`, exact set/binding/type/stage assertions).
- `rx_material/tests/test_material_system.cpp`: +1 case closing the
  pre-existing coverage gap the matrix's row 11 found (getPipeline's
  attachment-free rejection had zero test coverage before this round) —
  asserts the rejection is byte-identical, unchanged.

## Per-row proof (matrix, RC2/per-ticket-ruling-amended)

| # | Criterion | Disposition | Evidence |
|---|---|---|---|
| 1 | Compute compilation via existing path | Confirmed unmodified | `reflection_test.cpp` new case; `compileResult.entryPointCode[0].stage == VK_SHADER_STAGE_COMPUTE_BIT` asserted in 3 different test files |
| 2 | Compute reflection via existing path, incl. storage-buffer+image | Confirmed, **with a real fix** (see Deviations) | Same test; `RWStructuredBuffer`+`RWTexture2D` both reflect exact set/binding/type/stage |
| 3 | `PipelineLayoutBuilder::build()` reused unmodified for compute | Confirmed | `ComputePipelineCache::getOrCreate()` calls it verbatim; `compute_pipeline_test.cpp` |
| 4 | Bindless set 0 from compute | Confirmed, **live** | `test_compute_gpu.cpp` test 1: real bindless-registered storage buffer read inside the compute shader, its value folds into both outputs, asserted exactly |
| 5 | Barrier/PassSignature documentation correction | N/A (doc-only, Task 1's job) | Not this task's file to edit — noted, not actioned here |
| 6 | Compute PSO creation | Delivered | `rx::rhi::ComputePipelineCache` (rx_rhi_vk, parallel facility per ruling) |
| 7 | Storage-image (UAV) Pass API — **RC2-widened**: full per-mip/per-layer + cube/array | Delivered | See "What shipped" above; device-free tests + 2 real GPU tests |
| 8 | Architectural placement | Ruling followed exactly | `ComputePipelineCache` lives in `rx_rhi_vk`, sibling to `MaterialSystem`, reuses `PipelineLayoutBuilder` |
| 9 | Pipeline-cache persistence | Ruling's correction followed | Own `VkPipelineCache`, `MaterialSystem`'s exact load/save pattern, separate file |
| 10 | GPU test: buffer+image, exact values, graph-derived barriers, zero validation errors, both drivers | **Delivered, exceeded** (2 tests, +cube/array) | `test_compute_gpu.cpp`; lavapipe + real NVIDIA RTX 2080, zero unfiltered validation errors both |
| 11 | Attachment-free PassSignature accepted end-to-end; rejection test *added* (matrix corrected "updated" → "added") | Delivered | `test_material_system.cpp` new case (rejection unchanged, byte-identical); `test_compute_gpu.cpp`'s own attachment-free "produce" pass runs end-to-end via the new facility, never touching `MaterialSystem::getPipeline()` |
| 12 | Zero validation errors, sync validation infra reuse | Confirmed | All new GPU tests use the existing fixture/validation setup; `CHECK_FALSE(hasValidationErrors())` passes on both drivers |

Row 5 note: the matrix names this as a documentation-only correction to a
different task's registry text — not a code or test deliverable of this
ticket, left untouched.

Row 4 note on the matrix's own suggested "discrimination-style regression
asserts `stageFlags == VK_SHADER_STAGE_ALL`": Vulkan provides no query API
to read bindings back out of an opaque `VkDescriptorSetLayout`
(`pipeline_layout.h`'s own comment already documents this limitation for
an unrelated reason). A synthetic unit test cannot inspect this without
either duplicating `bindless.cpp`'s internal construction as a white-box
test or adding a test-only accessor with no production use. I verified the
source directly (`bindless.cpp:142,147,152,159`, all four bindings
`VK_SHADER_STAGE_ALL`, unchanged by this task) and rely on the *live* proof
the matrix itself calls decisive ("the new compute GPU test... successfully
samples/reads a bindless resource from the compute shader as live proof")
— test 1 does exactly this with an exact-value assertion, which is a
stronger, real signal than a stageFlags-equality check would have been.

## Deviations from the matrix's stated confidence

**Row 1/2 required a real fix, not just verification-only tests.**
Empirically found (not hypothesized) while writing this task's own GPU
test: `rx::shader::reflect()`'s second `getLayout()` call (compileImpl()
already calls `getLayout()` once internally, to map each entry point's
stage) crashed reliably — SIGFPE inside
`Slang::CompilerOptionSet::getProfile()`, then (after a partial fix)
SIGSEGV inside `Slang::TargetProgram::TargetProgram()` — for a **compute-
only** linked program, specifically inside a process that had already
initialized Vulkan/vk-bootstrap (every real GPU test fixture in this
codebase). Graphics (vertex+fragment) reflection was and remains
unaffected. Root cause: this shipped Slang release
(2026.14.1)'s `getLayout()` is not safely re-entrant for a compute-only
program under this condition. Fix: `CompileResult` gained a
`cachedLayout` field (`compiler.h`); `compileImpl()` stores its own
already-computed `ProgramLayout*` there; `reflect()` reuses it instead of
calling `getLayout()` a second time — eliminating the crash-prone
re-entrant call for *every* caller (graphics included), not a
compute-specific workaround. Also added (kept, not just a diagnostic
step) an explicit `CompilerOptionName::Profile` compiler-option entry
alongside the existing capability entry in `Compiler::create()`
(`compiler.cpp`) — empirically found necessary too (this alone took the
crash from SIGFPE to a later SIGSEGV, not to success; the `cachedLayout`
fix is what actually closes it, but the Profile entry is real,
independently-justified belt-and-suspenders now that `TargetDesc::profile`
is proven not to unconditionally populate the per-target-request state
`getTargetCaps()` reads).

Revert-discrimination performed directly (not just claimed): temporarily
reverted `reflect()` to always call `getLayout()` fresh (`cachedLayout`
bypass) — `test_compute_gpu.cpp` test 1 crashed with the exact original
SIGFPE, confirming the fix is load-bearing. Restored, rebuilt, reconfirmed
green on both presets and both drivers. Separately reverted the
subresource-overlap validator (`render_graph.cpp`) — the corresponding
`test_compile.cpp` case failed as expected (`CHECK(threw)` → false).
Restored, reconfirmed green.

**Storage-image usage flags.** `ImageDesc` (unlike `BufferDesc`) has no
caller-settable `usage` field — the render-graph Pass API models image
usage as purely kind-derived. `StorageImage::create()` therefore
unconditionally ORs in `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` (documented in
its own header) so any compute-written image can be read back/copied out
at all; without this the ticket's own required GPU test would have been
impossible to write. Not scope creep — a minimal, necessary completion of
row 7's own primitive, using the same "usage is unconditionally ORed in,
callers don't add it themselves" idiom `Texture2D::create()` already
established for its own direction (TRANSFER_DST for uploads).

**Scope boundary held, stated explicitly (not silently dropped):**
rasterized attachment output (`addColorOutput`/`setDepthStencilOutput`)
was **not** extended to carry subresource addressing — every resource
those kinds establish stays single-mip/single-layer, exactly as before
this task. No named Phase-5 consumer (T9 is compute-only per the plan
text; T22/T31 reuse T9's compute-class primitives per RC2/the T31 ruling)
needs a rasterized mip/cube attachment write. This mirrors the matrix's
own cited precedent (D9's GeometryPool defragmentation scoping) for
shipping the minimal shape a named consumer needs.

## Both-preset / both-driver verification (command tails)

Lavapipe, full suite, linux-native (final run after fix restore):
```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  80.94 sec
```

Windows-cross-zig, full build + Wine ctest (CI's own exclusion pattern):
```
ninja                                    # 22/22, zero errors
xvfb-run -a ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample' -j1
...
100% tests passed, 0 tests failed out of 13
Total Test time (real) = 97.62 sec
```

Real driver (NVIDIA GeForce RTX 2080, driver 580.82.07, default ICD —
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json`, `DISPLAY=:1`,
on-desktop, serialized/NICEd per the standing owner rule):
```
rx_graph_gpu_tests:   15/15 test cases, 1169/1169 assertions, SUCCESS
rx_rhi_vk_tests:      90/90 test cases, 2314/2314 assertions, SUCCESS
rx_material_gpu_tests: 57/57 test cases, 2546/2546 assertions, SUCCESS
```
Every "[vulkan validation] Error"/"Warning" line observed on either
driver across the whole session matches this codebase's own pre-existing,
documented false-positive filter set (`context.cpp`'s `debugCallback()` —
the `VK_KHR_portability_enumeration` instance-flags message and the
Slang `OpSource` SourceLanguage=11 message, both logged with an explicit
"(known false positive: ...)" prefix by that filter itself); zero
unfiltered errors on any run (`hasValidationErrors()` false / process
tally zero throughout).

## Self-review

- **TDD discipline**: the resource-model extension (resources.h/pass.h/
  render_graph.cpp/barriers.cpp) was implemented then immediately
  test-driven at the device-free layer before any RHI/executor code
  existed to consume it — genuine failing-test iteration happened there
  (e.g. my first draft of the barrier-independence tests asserted "no
  barrier on first use," which is wrong for images; the real Vulkan
  behavior — first use always transitions layout — corrected both the
  test and my understanding, which is exactly what TDD is for). The
  Slang crash was found via the GPU test I wrote *before* it passed, not
  discovered after the fact.
- **No deferred fixes**: the Slang crash is fixed, not routed around or
  filed as a follow-up — it would have blocked every real production
  compute consumer (T9/T14/T22/T31), not just this ticket's own test.
- **Scope discipline**: RC2's widened scope is delivered in full (cube/
  array/per-mip/per-layer, generic, reusable by T9/T22/T31 without
  further extension) while explicitly NOT extending rasterized attachment
  output, which nothing currently named needs — stated, not silent.
- **No AI attribution**: none added anywhere (commit messages, code
  comments, this report).
- **Commit scope**: pathspec-scoped to exactly the files listed under
  "What shipped" above; `.superpowers/sdd/2026-08-20-phase5-techniques/
  progress.md` is being concurrently modified by another active agent
  (per `git status` at session start/throughout) and is deliberately
  excluded from this commit.
- **Concerns for the coordinator**: (1) the Slang 2026.14.1 reflect()
  re-entrancy defect is now worked around at the `rx_shader` API level,
  but the underlying vendored library behavior is unverified against any
  *newer* Slang release — worth a note in the vendoring/dependency
  tracking if one exists; (2) the subresource-overlap validator's
  identical-or-disjoint restriction is a real, load-bearing scope
  decision for T9/T22/T31 to know about before they design their own
  pass topologies (documented in resources.h/render_graph.cpp and this
  report, not hidden).
