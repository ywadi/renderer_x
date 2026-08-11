# Task 4 review: material texture-sampling wiring

Reviewed: commit `354b8d5` (`feat: wire real bindless texture sampling into material shaders`) on worktree branch
`worktree-agent-aa361cab989151211`, report at `.superpowers/sdd/2026-08-11-phase4-scene-assets/task-4-report.md`
(in-branch). Scope: this single commit's own content, weighed against `task-4-brief.md` and the chain it closes
(`.superpowers/sdd/2026-08-10-phase3-render-graph-materials/task-8-review.md`, "The central question: texture path
vs. texture sampling"). Verification performed directly against the worktree checkout (not just the diff/report):
full source reads of every changed function (`reflectMaterialLayout()`, `bindInstance()`, `MaterialSystem::create()`
in `material_system.cpp`; the new bindless declarations in `material.slang`; `forward_entry.slang`'s vertex path;
`bindless.h`'s binding constants), plus independent command execution on this machine — re-ran the existing
`RX_TRACY=ON` build (`rx_material_gpu_tests`, both the system validation layer and the "newer" layer at
`/home/ywadi/sponza/vvl`), configured and built a fresh `RX_TRACY=OFF` tree from the worktree source and re-ran the
same suite, and re-ran `sample_06_materials --validate`. Cleaned up the extra build directory afterward.

## Spec compliance verdict: ✅

| # | Requirement | Verdict | Evidence |
|---|---|---|---|
| 1 | `material.slang`/`forward_entry.slang`: bindless texture+sampler array access, `rx_sampleTexture(uint index, float2 uv)` helper | ✅ (see note below) | `material.slang` declares `[[vk::binding(0,0)]] Texture2D gTextures[]`, `[[vk::binding(1,0)]] SamplerState gSamplers[]`, a `gMaterialGlobals` push constant, and `public float4 rx_sampleTexture(uint textureIndex, float2 uv)` calling `gTextures[textureIndex].Sample(gSamplers[gMaterialGlobals.defaultSamplerIndex], uv)`. `forward_entry.slang` itself is untouched — correctly so: it never samples a texture, only material modules do, and every material module already `import material;`, so the globals belong where they're declared. The brief's file list names both files as candidates for modification; only one turned out to need it, explained rather than silently skipped. |
| 2 | `reflectMaterialLayout()` allow-list accepts material-side bindless references | ✅ | Verified by direct code read (see "Scrutiny" below): the walk now recognizes exactly two additional shapes (the set-0 `DescriptorTableSlot` pair at `BindlessTable::kSampledImageBinding`/`kSamplerBinding`, unbounded, correctly-typed; a `PushConstantBuffer` named exactly `RxMaterialGlobals` sized exactly `sizeof(uint32_t)`) and rejects everything else, including binding 2 (the table's real storage-buffer binding) and any bounded/mistyped variant. |
| 3 | Tests: `+tests/data/test_textured_sample.slang` | ✅ | New fixture whose `evaluate()` calls `rx_sampleTexture()` directly (unlike Task 7's `test_textured.slang`, kept as the deliberate "path but not sampling" case). |
| 4 | Acceptance bar: `createTexture2D`→`setTexture` visibly changes rendered output; GPU test renders a quad with a 2×2 texture through the public API, asserts 4 quadrant colors | ✅ | Independently reproduced (see below): both new `test_api_factory.cpp` GPU tests pass with byte-exact quadrant matches; the render path goes through the real public `createTexture2D`/`setTexture`, only dropping to the internal `MaterialSystem::bindInstance()` bridge for the draw call itself — the same public/internal split Phase 3's sample 06_materials established, not a shortcut invented here. |
| 5 | Hot-reload of a textured material keeps working | ✅ | Reproduced directly: the hot-reload test writes v1, renders, edits to a content-different v2, calls the public `reloadChanged()`, confirms a real hash change, re-renders, and the SAME quadrant colors survive. My own run logged the real hash transition (`0xba079f66... -> 0x74676523...`) and all quadrant checks passed. |
| 6 | GUID regen per policy | ✅ | `rx_api.h` is not in the diff (confirmed via `git show 354b8d5 --stat`); no ABI-visible shape changed; no regen action needed, correctly stated as such. |
| 7 | Steps: failing test → implement → suite green both presets, zero validation errors → commit | ✅ (isolation-qualified, see Finding-adjacent note) | Both presets build clean per the report (Linux half independently confirmed by rebuilding). "Zero validation errors" holds unconditionally once isolated from one pre-existing, out-of-scope defect (Tracy's GPU-context command pool) — independently reproduced below, not taken on trust. |

## Scrutiny

**1. Allow-list relaxation — surgical, verified by direct read, not by re-reading the report's own characterization.**
Read `reflectMaterialLayout()` in full (`src/rx_material/material_system.cpp`). For a `DescriptorTableSlot`-category
global to be accepted, ALL of the following must hold simultaneously: `set == 0`, `bindingIndex` exactly equal to
`BindlessTable::kSampledImageBinding` (0) or `kSamplerBinding` (1), the element kind classified by
`classifyBindlessArrayElement()` matches exactly (`SamplerState` → sampler array; a `Texture2D`-shaped, non-combined
`Resource` → sampled-image array — anything else, including a combined-sampler texture or a 3D/cube shape, is
`Other`), and `isReflectedAsUnboundedArray()` confirms genuine `SLANG_UNBOUNDED_SIZE` (a bounded array of the right
shape is rejected). Any global that fails any one of these — including one declared at `BindlessTable`'s real
binding 2 (`kStorageBufferBinding`, confirmed at `bindless.h:161`), a wrong set, a bounded count, or a
type-mismatched resource at binding 0/1 — falls through to the same generic "declares an unsupported bindless
global" rejection the pre-Task-4 code used for everything. The `PushConstantBuffer` branch is equally narrow: exact
element-type name `RxMaterialGlobals` and exact reflected size `sizeof(uint32_t)`, both checked before acceptance.
The pre-existing Phase 3 rejection paths this scrutiny asked about — `IRxMaterialInstance::setFloat/setFloat4/
setTexture` rejecting wrong-type/wrong-name field access on a material's own `gParams` (`test_api_factory.cpp`'s
"happy path... validates" test: `setFloat("tint", ...)` → `RX_E_INVALIDARG`, `setFloat("nonexistent", ...)` →
`RX_E_NOTFOUND`) — are untouched by this diff (confirmed: the diff's hunks to `test_api_factory.cpp` are additive
only, no existing `TEST_CASE` body was edited) and still pass in my own re-run.

**2. Sampler convention — a single, eagerly-created, push-constant-carried index; documented where a material
author would actually look.** `MaterialSystem::create()` creates and registers exactly one default
LINEAR/CLAMP_TO_EDGE sampler via `bindless.registerSampler()` before the `MaterialSystem` object is even returned —
there is no code path that can call `bindInstance()` (which requires a live `MaterialSystem`) before this sampler
exists, so "a material sampling before any sampler exists" cannot occur. `bindInstance()` writes the sampler's
*real* registered `BindlessHandle::index()` into the `gMaterialGlobals` push-constant range on every draw-time bind,
guarded on `record->layoutInfo.pushRanges` being non-empty (verified this guard is a real check, not a rubber stamp:
the push range's presence comes from the SAME reflection pass that also reflects each material's own bindings, so
it can never desync from what the pipeline layout actually declares). `kMaterialStageFlags` (vertex|fragment,
`material_system.cpp:117`) covers the fragment stage where `evaluate()`/`rx_sampleTexture()` actually run. The
choice and its rationale live in `material.slang`'s own header comment directly above the declarations — the file
every material module must `import`, so this is the natural place a material author reading that module would find
it, not a buried internal comment. There is no separate materials-authoring guide yet (no sample currently calls
`rx_sampleTexture`), which is a defensible, disclosed gap for this phase rather than a defect — see Finding F1.

**3. Quadrant test — genuinely public-API end-to-end, and the probe math is sound, not hand-waved.** Traced the
call chain in `test_api_factory.cpp`: `IRxMaterialSystem::createTexture2D()` (public) → `IRxMaterialInstance::
setTexture()` (public) → drops to the internal `MaterialSystem::bindInstance()` bridge only for the draw submission
itself, via the SAME `rx::material::detail::materialHandle`/`materialInstanceBlobData` accessors sample
06_materials' own bridge already established (`rx_api.h` deliberately does not expose draw submission this phase —
an existing, not newly-invented, architecture boundary). Reworked the bilinear-filtering math by hand for the probe
positions: with `CLAMP_TO_EDGE` addressing and standard half-texel bilinear sampling against a 2-texel-wide texture,
a UV component in `[0, 0.25]` or `[0.75, 1]` always samples two texel indices that both clamp to the SAME real texel
(index −1 and 0 both clamp to 0, or index 1 and 2 both clamp to 1), making the blend weight irrelevant and the
result byte-exact — the probes at 1/8 and 7/8 fractions (pixel centers at `u≈0.133`/`0.867` for a 64px extent) sit
comfortably inside that band with margin, not at a marginal edge case. Independently confirmed empirically too: both
new GPU tests pass their `memcmp`-exact quadrant checks in my own re-run.

**4. Hot-reload with textures — survives because the instance blob and the material's compiled pipeline are
independently-owned objects.** The bindless texture index lives in the `IRxMaterialInstance`'s own CPU-side
parameter blob (written once by `setTexture()`), which `reloadChanged()` never touches — only the material's
Slang program/pipeline/reflected layout gets recompiled. `MaterialSystem::Impl::defaultSampler`/
`defaultSamplerHandle` are created once in `create()` and are similarly untouched by `reloadChanged()`. The new
regression test doesn't just assert this abstractly — it forces a real content-hash change (v1→v2, textually
different, semantically identical) and confirms the SAME quadrant colors survive a genuine recompile. Reproduced
directly: the reload logged a real hash transition and both pre/post renders matched byte-exact.

**5. UV provenance — real per-vertex data, not zeros.** Read `forward_entry.slang`'s `vertexMain(float3 position :
POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)` directly: `uv` is a genuine vertex input attribute,
passed through `VertexStageOutput.uv` to `fragmentMain`'s `MaterialVertex v.uv` unmodified — no zeroing, no
placeholder. Cross-checked the test's `QuadTestVertex{position[3], normal[3], uv[2]}` against
`material_system.cpp`'s `MaterialVertexLayout`/`makeVertexInputState()` (identical field order/offsets, location
0/1/2) — the test's real per-corner UVs (`(0,0)/(1,0)/(1,1)/(0,1)`) are the actual values the fragment shader reads.

**6. Isolation methodology — independently reproduced from scratch, not taken on the report's word.** Ran the
existing `RX_TRACY=ON` build myself: `rx_material_gpu_tests --validate` → **23/27 passed, 4 failed**, both against
the system validation layer and against `/home/ywadi/sponza/vvl`'s newer layer — identical result both times. All 4
failures are, in my own output, the SAME single assertion (`CHECK_FALSE(fixture->context.hasValidationErrors())`)
tripped by the SAME `VUID-vkBeginCommandBuffer-commandBuffer-00050`; none of the functional pixel/hash assertions in
this task's own two new tests failed. Configured and built a fresh `-DRX_TRACY=OFF` tree from the same worktree
source (`cmake -S . -B build/linux-native-tracy-off ... -DRX_TRACY=OFF`, built only the two affected targets) and
re-ran: **27/27 passed, 0 failures**, `rx_material_tests` 10/10. Also re-ran `sample_06_materials --validate`: all 4
checker/rim objects `matched=true`, confirming no regression to non-texture materials from the reflection change.
Independently read the root cause too, not just the symptom: `src/rx_rhi_vk/src/tracy_gpu.cpp`'s command pool
(pre-fix, as still checked out in this worktree) creates with only `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT`, and the
vendored `TracyVulkan.hpp`'s `VkCtx` constructor calls `vkBeginCommandBuffer` on the SAME command buffer up to three
times (lines 144/153/162) without an explicit reset — exactly the precondition for VUID-00050 on a pool missing
`RESET_COMMAND_BUFFER_BIT`. This mechanistic read confirms the report's diagnosis independent of trusting it.
Further corroboration found live during this review: the main branch (currently at `77bf292`, landed after Task 4's
own commit) already carries the exact one-line hotfix the report recommended, with a commit message that
explicitly credits "Task 4's implementer building a real Executor under RX_TRACY=ON with validation" for the find —
an independent, authoritative confirmation that the diagnosis was correct and has since been acted on. This
pre-existing defect is not scored against Task 4: it is outside Task 4's file scope (never touches
`src/rx_rhi_vk/`), was not introduced by this diff, was disclosed prominently rather than hidden, and is now fixed
on main via a separate, already-landed commit.

**7. Attribution / newer-layer evidence.** `git show 354b8d5 --format="%an <%ae>%n%cn <%ce>%n%B"` and a full-diff
grep for AI-attribution markers (`claude`, `anthropic`, `co-authored`, `generated by`, `ai assist`, etc.) return no
hits; author/committer is the human user's own identity. The "newer" VVL layer path
(`/home/ywadi/sponza/vvl`) exists on this machine and was independently exercised (item 6 above), reproducing the
report's claimed identical 23/27 result.

## Quality verdict: Approved — 1 finding, Low severity

- **F1 (Low, documentation/discoverability):** the engine's one process-wide default sampler recipe (LINEAR +
  CLAMP_TO_EDGE, no per-material override yet) is documented only in-source — `material.slang`'s header comment
  and `material_system.cpp`'s `defaultSamplerInfo` comment — with no materials-authoring-facing doc yet (no sample
  currently calls `rx_sampleTexture`, so there is nothing to update there today). This is a reasonable, disclosed
  scope boundary for this task, not a defect, but worth a backlog entry so a future material author relying on
  tiling (REPEAT) doesn't discover the CLAMP_TO_EDGE default only by seam artifacts, and so Phase 4's StandardPBR
  work inherits this as a stated convention rather than tribal knowledge.

No correctness, ABI-discipline, reflection-safety, or test-integrity defects found. Every item flagged for hardest
scrutiny in this task's dispatch (allow-list surgical-ness, sampler registration/documentation, quadrant-probe
rigor and public-API discipline, hot-reload survival, UV provenance, isolation-methodology soundness) was
independently re-verified against the actual code and by direct re-execution on this machine, including a
from-scratch `RX_TRACY=OFF` rebuild — not taken on the report's word alone.
