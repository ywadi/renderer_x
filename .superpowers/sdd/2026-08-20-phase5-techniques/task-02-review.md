# Review — Task 2 (#38): Compute pipeline capability + RC2 resource-model extension

Reviewer round. Commit under review: `00bb451` (single commit, parent `1c01881`).
Order of authority followed: `rulings-2026-08-20.md` > plan > `matrix-p5t02-compute-pipeline.md`
> ticket #38. Independent of the implementer; all findings below are from
direct code reading, diff inspection, and reproduced build/test evidence,
not from re-trusting the implementer's report.

## Verdict 1 — Spec compliance: **PASS**

All matrix rows (as amended by RC2 + the T2 per-ticket ruling) are satisfied
by the actual code, verified directly, not merely as claimed in the report:

- **ComputePipelineCache is a parallel `rx::rhi` facility, not inside
  MaterialSystem.** `src/rx_rhi_vk/include/rx_rhi_vk/compute_pipeline.h` +
  `src/rx_rhi_vk/src/compute_pipeline.cpp`; `git diff 1c01881 00bb451 --
  src/rx_material/material_system.cpp src/rx_material/include/rx_material/
  material_system.h` is **empty** — zero lines touched in MaterialSystem.
- **`getPipeline()`'s attachment-free rejection is byte-identical,
  unchanged**, and is now covered by a real, discriminating negative test
  (`test_material_system.cpp:1539-1568`: constructs the exact degenerate
  `PassSignature{}` — `colorCount==0`, `depthFormat==VK_FORMAT_UNDEFINED` —
  and asserts both the throw and the message content).
- **No PassSignature in the compute cache key.** `ComputePipelineCache`'s
  key is `hashSpirv()` (FNV-1a-64 over the raw SPIR-V words) alone; `Entry`
  holds only `module`/`layoutBundle`/`pipeline` — no attachment-shape field
  anywhere in the key or the cached entry.
- **Storage-image Pass API mirrors the StorageBuffer Pass API shape** with
  no drift a consumer (T9/T22/T31) would trip on — side by side:
  ```cpp
  Pass& addStorageBufferOutput(std::string_view name, const BufferDesc& desc);
  Pass& addStorageBufferInput(std::string_view name);
  Pass& addStorageImageOutput(std::string_view name, const ImageDesc& desc, Subresource subresource = {});
  Pass& addStorageImageInput(std::string_view name, Subresource subresource = {});
  ```
  Same naming, parameter order, fluent `Pass&` return; the only addition is
  a trailing defaulted `Subresource` (necessary — buffers have no mip/layer
  concept; source-compatible default = whole resource).
- **ComputePipelineCache reuses `PipelineLayoutBuilder::build()` verbatim**
  (`compute_pipeline.cpp:109`) and **owns its own separate `VkPipelineCache`**
  using MaterialSystem's exact load/persist pattern (load-if-present,
  `vkGetPipelineCacheData` + write-to-disk at teardown, warn-not-fatal on a
  malformed cache) — confirmed line-for-line equivalent logic against
  `material_system.cpp`'s own load/save code, differing only in log prefix.
- **`Texture2D`/`texture.{h,cpp}` untouched** — `git diff` on those files is
  empty; `StorageImage` is a genuinely new, purpose-built sibling.

**RC2 (resource-model extension) — delivered in full, once, in this
ticket, as ruled:**

- `Subresource`/`ImageDesc`/`PhysicalResource::mipLevels/arrayLayers/cube`
  land in `resources.h`; sentinel resolution in `render_graph.cpp`
  resolves the default `{0, kRemaining, 0, kRemaining}` against a
  pre-existing single-mip/single-layer resource to exactly `{0,1,0,1}` —
  verified byte-identical in effect to the old `VK_REMAINING_*` behavior
  (confirmed in-code comment, `executor.cpp:567-577`, and by the full
  pre-existing test suite passing unchanged on both drivers).
- Barrier state is keyed by `(physicalIndex, Subresource)`
  (`barriers.cpp:184-244`) with field-wise `operator==` — two different
  mips of the same physical resource get **genuinely independent** barrier
  state. Write-mip-N / read-mip-N+1 correctly gets **no false
  serialization** (disjoint ranges → separate fresh state machines) and
  **no missed hazard** (this task's scope never implies a mip-chain
  generation dependency — that is explicitly future work, e.g. T22 — so
  independence here is the *correct* behavior, not a gap).
- **Identical-or-disjoint validator FAILS LOUDLY on true partial overlap,
  confirmed with a genuine (non-degenerate) case.**
  `render_graph.cpp:832-874` — standard half-open-interval AABB overlap
  (`a.start < b.end && b.start < a.end`) applied independently to mip and
  layer ranges, `throw std::runtime_error` naming the resource on overlap.
  `test_compile.cpp:903-925` exercises "whole array at mip 0" (layers
  `[0,6)`) vs "layer 3 alone" (layers `[3,4)`) — a real containment overlap
  on the layer axis, neither identical nor disjoint; this is a
  discriminating test (weakening the check would make it fail to throw).
  Companion disjoint-mip and identical-range tests confirm no false
  positives.
- Cube validation (`arrayLayers` must be a positive multiple of 6),
  `PooledStorageImage`/`TransientPool::acquireStorageImage()`,
  `Executor::realize()` routing `VK_IMAGE_USAGE_STORAGE_BIT` resources to
  the new pool, and `applyBarriers()` threading the resolved `Subresource`
  into the real `VkImageMemoryBarrier2::subresourceRange` (replacing the
  hardcoded `VK_REMAINING_*`) — all confirmed present and correctly wired.

## Verdict 2 — Code quality: **Approved**, with one low-severity finding

### Findings

1. **[Low] Test-coverage gap: the subresource-overlap validator's
   device-free tests exercise a genuine partial overlap only on the
   *layer* axis, never the *mip* axis.** `render_graph.cpp:847-851`
   computes `mipOverlaps`/`layerOverlaps` with the identical formula
   applied to each dimension (manifestly symmetric and correct by direct
   inspection), but `test_compile.cpp`'s three overlap-family cases cover
   only: layer-axis partial overlap (throws, line 903), fully-disjoint
   mips (accepts, line 927), and fully-identical ranges (accepts, line
   939) — no case constructs a genuine mip-axis-only partial overlap (e.g.
   mips `[0,2)` vs `[1,3)`, same layers). Not a functional defect — the
   code path is generic across both axes, not duplicated/forked logic —
   but a future regression isolated to the mip-comparison branch (e.g. an
   accidental `<=`/`<` swap) would slip past this suite undetected.
   Recommend a follow-up test before/alongside T9 or T22 (the next real
   mip-chain consumer), not a blocker for this gate.

No other findings. Every new test case inspected (`test_compute_gpu.cpp`
x2, `test_barriers.cpp` x2, `test_compile.cpp` x11,
`compute_pipeline_test.cpp`, `storage_image_test.cpp`,
`reflection_test.cpp`, `test_material_system.cpp`) asserts real,
discriminating outcomes (exact values, handle identity for cache-hit/view-
cache proofs, or throws) — no vacuous "didn't crash" assertions found
anywhere in this commit.

## The Slang workaround — explicit verdict

**Verdict: sound, load-bearing, correctly scoped. Approved.**

- **Mechanism, verified by direct read:** `compileImpl()`
  (`compiler.cpp`) already called `linkedProgram->getLayout(0, ...)` once,
  internally, for every compile (to map each entry point's stage).
  `reflect()` (`reflection.cpp`) previously called `getLayout()` again, on
  the same `linkedProgram`, at the same `targetIndex=0`. The fix stores
  `compileImpl()`'s own already-computed `ProgramLayout*` in a new
  `CompileResult::cachedLayout` field (`compiler.h:55`) and `reflect()`
  reuses it (`reflection.cpp:162`), with a defensive fresh-`getLayout()`
  fallback that is dead code on every real production path (`compileImpl()`
  always sets `cachedLayout` alongside `linkedProgram`, and `reflect()`'s
  own null-guard rejects anything else before that fallback could run).
- **(a) Graphics-path behavioral identity: PLAUSIBLE, strongly evidenced,
  not formally provable from this repo.** Both the old first call
  (inside `compileImpl()`) and the old second call (inside `reflect()`)
  invoke `getLayout()` on the identical component-type object at the
  identical target index. Slang's vendored header describes `getLayout()`
  as a layout query with no documented invalidation/side-effect semantics
  between calls. Slang itself is shipped prebuilt (no vendored source in
  this tree), so idempotency cannot be proven from source directly — the
  evidence bar met here is the correct one: the entire pre-existing
  graphics reflection test suite (`reflection_test.cpp`'s 4 original
  cases, unmodified) plus every graphics-path GPU test in the repo passed
  unchanged on **both drivers** in this review's own full-suite reruns
  (see Empirical verification below). The crash itself is also
  independent evidence *for* equivalence, not against it — it manifests
  as a low-level `SIGFPE`/`SIGSEGV` inside Slang's own
  `TargetProgram`/`CompilerOptionSet` re-entrant-construction machinery,
  not as two different reflection outputs.
- **(b) Revert discrimination — reproduced independently by this review,
  not just re-trusted from the report.** See Empirical verification: 1
  edit (`cachedLayout` bypass), rebuild, targeted run on real NVIDIA →
  **SIGSEGV**, exact test named `compute-only reflect() succeeds inside a
  process that has already initialized Vulkan/vk-bootstrap`. Restored
  byte-identically (`git diff` empty), rebuilt, reconfirmed green.
- **(c) Lifetime — safe.** `cachedLayout` is a raw `slang::ProgramLayout*`
  whose validity is explicitly documented as anchored to
  `linkedProgram`'s (`Slang::ComPtr<slang::IComponentType>`, refcounted)
  lifetime; both fields are written together, only inside `compileImpl()`.
  Every `reflect()` call site in the repo (rx_shadow, rx_asset,
  rx_material, rx_rhi_vk, rx_graph tests) calls it synchronously in the
  same scope the `CompileResult` was just constructed in — no call site
  stores/moves a `CompileResult` (or just its `cachedLayout`) across a
  boundary where `linkedProgram` could already have been released.

## Empirical verification performed by this review

All commands run from the real repo path (`cd -P`), NICEd (`nice -n 10`),
foreground, one at a time.

- **Full serial lavapipe ctest, before any sabotage:** `100% tests passed,
  0 tests failed out of 29` (79.34s). Matches the implementer's claim.
- **Three GPU test binaries on real NVIDIA (GeForce RTX 2080, driver
  580.82.07, `VK_ICD_FILENAMES=.../nvidia_icd.json`, on-desktop `DISPLAY=:1`):**
  - `rx_graph_gpu_tests`: 15/15 test cases, 1169/1169 assertions, SUCCESS.
  - `rx_rhi_vk_tests`: 90/90 test cases, 2314/2314 assertions, SUCCESS.
  - `rx_material_gpu_tests`: 57/57 test cases, 2546/2546 assertions, SUCCESS.
  - Total 162/162 test cases — **matches the implementer's claim exactly.**
    Every "Validation Error/Warning" line observed is one of the
    codebase's own pre-labeled known-false-positives (`VK_KHR_
    portability_enumeration`, Slang `OpSource` SourceLanguage=11); zero
    unfiltered validation errors on either driver.
- **Slang revert re-proof (real NVIDIA driver):** temporarily replaced
  `slang::ProgramLayout* layout = result.cachedLayout;` with
  `layout = nullptr;` in `reflection.cpp` (forcing the fresh-`getLayout()`
  fallback path unconditionally), rebuilt, ran the exact regression test
  (`rx_rhi_vk_tests`, `compute-only reflect() succeeds inside a process
  that has already initialized Vulkan/vk-bootstrap`) → **`FATAL ERROR:
  test case CRASHED: SIGSEGV`**, reproducing the defect. Restored the
  file; `git diff` confirmed empty (byte-identical); rebuilt; re-ran →
  1/1 test case, 6/6 assertions, SUCCESS.
- **Value-assertion sabotage re-proof (real NVIDIA driver):** in
  `test_compute_gpu.cpp`'s embedded compute shader source, changed
  `[numthreads(8, 8, 1)]` to `[numthreads(4, 4, 1)]` (workgroup-math
  sabotage; dispatch count left at `(1,1,1)`, so only 16 of 64
  buffer/image elements are actually written by the shader). Rebuilt, ran
  the primary dispatch test → **both `CHECK(allBufferValuesCorrect)` and
  `CHECK(allImageValuesCorrect)` failed** exactly as expected (`test_
  compute_gpu.cpp:513`, `:526`), 2/644 assertions failed, test case
  FAILURE — confirming these are real, discriminating value assertions,
  not theater. Restored the file; `git diff` confirmed empty; full
  rebuild; full lavapipe ctest re-run → 29/29 passed; both touched real-
  driver GPU binaries re-run → `rx_graph_gpu_tests` 15/15/1169 and
  `rx_rhi_vk_tests` 90/90/2314, both SUCCESS, matching the original counts
  exactly.
- **Commit hygiene:**
  - Single commit (`00bb451`, parent `1c01881`, matches the diff under
    review and the implementer's stated base) spanning `rx_graph`,
    `rx_rhi_vk`, `rx_shader`, and one `rx_material` test addition.
    **Assessed as acceptably scoped, not a finding**: RC2 explicitly rules
    that the resource-model extension lands "ONCE" in this ticket, and the
    Slang fix was discovered empirically *while writing this ticket's own
    required GPU test* (not an unrelated task bolted on) — the multi-
    subsystem span is a direct, ruling-mandated consequence of one
    ticket's scope, not scope creep or an unrelated batch commit.
  - Author/committer: `Yousef Wadi <ywadi85@gmail.com>`, matching local
    git config — not overridden.
  - `git show 00bb451 | grep -iE "claude|anthropic|co-authored-by|
    generated by|ai assistant|chatgpt|copilot"` — **zero matches**, in
    either the commit message or the full diff body.
  - Not pushed: `git status` shows `ahead of 'origin/main' by 1 commit`.
  - Working tree after this review's restores: `git diff --stat` shows
    only the pre-existing, out-of-scope `.superpowers/sdd/2026-08-20-
    phase5-techniques/progress.md` modification (5 insertions, unrelated
    to this ticket, left untouched per instructions) — no residue from any
    temporary review edit.

## Not independently verified by this review

- The implementer's Windows-cross-zig claim (`22/22` build, `13/13` ctest
  under Wine) — outside this review's stated empirical minimum (lavapipe +
  real-NVIDIA + Slang revert + one sabotage + commit hygiene); not
  reproduced here. No reason from the diff to doubt it, but flagged as
  implementer-claimed only.
- The precise Slang idempotency guarantee for `getLayout()` (point (a)
  above) cannot be proven from source since the vendored Slang release is
  a prebuilt binary with no accompanying source in this tree — treated
  as PLAUSIBLE-BUT-NOT-FORMALLY-PROVEN throughout, on the strongest
  evidence available (full existing regression suite unchanged on both
  drivers, documented pure-query API contract, identical call arguments
  before and after).
