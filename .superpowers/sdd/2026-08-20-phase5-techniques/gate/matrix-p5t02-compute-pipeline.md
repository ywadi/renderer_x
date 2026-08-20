# Matrix — Issue #38 (P5 T02): Compute pipeline capability (PSO + dispatch + reflection)

**Ticket:** #38 "[P5 T02] Compute pipeline capability (PSO + dispatch + reflection)" (`phase-5`, `stage-0`).
**Plan task:** Task 2, `docs/superpowers/plans/2026-08-20-phase5-techniques.md:173-205` (Stage 0).
**Spec/registry decisions binding this ticket:**
- Registry line "Compute pipeline capability" (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:246-259`)
  — committed 2026-08-18 in the geometry phase, pulled forward to Phase 5
  Stage 0; names three already-committed consumers (compute-culling as the
  mesh-shader baseline, compute pre-skinning, GPU-driven culling) and states
  "the render graph's compute-class pass and barrier machinery are
  delivered (Phase 3; compute-class barrier stages derive from
  attachment-free signatures)" — **verified below to be accurate in effect
  but imprecise in mechanism; see row 5 and the Conflicts section.**
- Plan Global Constraints (`plan.md:55-111`), specifically: real-GPU
  verification (lavapipe + real driver, both mandatory), reference-vs-
  ground-truth discipline (VALUE assertions, not execution-only), no
  deferred fixes, D5 threading contract, and the standing rule "Compute
  passes go through the render graph's compute-class pass machinery
  (barriers derive from attachment-free signatures — delivered Phase 3); no
  hand-rolled dispatch outside the graph in production paths" (`plan.md:
  109-111`) — this last constraint is **load-bearing for row 7 below**: it
  forecloses the option of writing to a compute-created storage image
  outside graph tracking, which is exactly why the storage-image gap found
  in this session (row 7) cannot be worked around informally.
- **No port-source dependency**: the ticket's own body text ("Scope")
  explicitly calls this "thin RHI surface over volk/VMA/the existing
  pipeline cache — no ready-made library applies beyond what is already
  adopted (explicit from-scratch call, recorded per CLAUDE.md)". This
  matrix therefore does not carry first-tier-engine precedent citations for
  the PSO-creation mechanics themselves (unlike every other Stage-0
  ticket) — only direct code verification and Vulkan-spec-level facts.

**Sources consulted (all first-hand this session, repo HEAD `bf5b853`):**
- `gh issue view 38` (2026-08-20) — full ticket body, verbatim acceptance
  sketch.
- `docs/superpowers/plans/2026-08-20-phase5-techniques.md` — "Global
  Constraints" (lines 55-111, full) and "Task 2" (lines 173-205, full).
- `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`
  lines 240-264 (registry entry) and lines 364-459 (techniques-phase
  charter block).
- `src/rx_graph/include/rx_graph/pass_signature.h` (102 lines, full read).
- `src/rx_graph/include/rx_graph/pass.h` (369 lines, full read).
- `src/rx_graph/include/rx_graph/resources.h` (172 lines, full read).
- `src/rx_graph/barriers.cpp` (256 lines, full read).
- `src/rx_graph/render_graph.cpp` lines 100-320 and 630-715 (the
  `hasAttachmentOutput()`/`resolveAccess()`/compile-time `AccessKind`
  switch regions — the exact code that resolves stages/access/layout AND
  unions `PhysicalResource::imageUsage`).
- `src/rx_graph/executor.cpp` lines 1370-1440 (PassSignature construction
  inside `execute()`).
- `src/rx_graph/include/rx_graph/executor.h` lines 295-340 (`PassContext::
  passSignature()`'s own doc comment, which explicitly names
  `MaterialSystem::getPipeline()`'s rejection).
- `src/rx_graph/transient_pool.h` (`acquireImage()`/`acquireHistory()`
  signatures — confirms `VkImageUsageFlags` flows through verbatim to pool
  image creation, so a `PhysicalResource::imageUsage` fix is sufficient with
  no pool-level change needed).
- `src/rx_shader/src/compiler.cpp` (351 lines, full read).
- `src/rx_shader/src/reflection.cpp` (291 lines, full read).
- `src/rx_shader/tests/reflection_test.cpp` (219 lines, full read — 4 test
  cases, all vertex/fragment only, zero compute coverage).
- `src/rx_rhi_vk/include/rx_rhi_vk/pipeline_layout.h` (163 lines, full
  read) + `src/rx_rhi_vk/src/pipeline_layout.cpp` (270 lines, full read).
- `src/rx_rhi_vk/src/bindless.cpp` lines 120-190 (set-0 layout creation,
  all four bindings' `stageFlags`).
- `src/rx_material/material_system.cpp` lines 100-140 (`kMaterialStageFlags`
  + surrounding context), lines 1900-2090 (`getPipeline()` in full: the
  attachment-free rejection, the hardcoded 2-stage
  `VkPipelineShaderStageCreateInfo` array, fixed-function state derivation,
  `vkCreateGraphicsPipelines` + `impl.pipelineCache` use), lines 1350-1560
  (`VkPipelineCache` creation/load/persistence — confirmed the ONLY
  `VkPipelineCache` instance anywhere in this codebase).
- `src/rx_material/include/rx_material/material_system.h` lines 460-500
  (`bindInstance()`'s own doc comment — confirms the "caller binds
  BindlessTable's set 0 once per frame/pass" pattern this ticket's compute
  path must replicate, and confirms set 1 material-instance binding is
  graphics/material-instance-specific, not something a general compute
  kernel needs).
- `src/rx_material/tests/test_material_system.cpp` — grepped exhaustively
  for `colorCount`, `getPipeline`, `CHECK_THROWS` near the attachment-free
  rejection: **zero hits** on any test exercising
  `colorCount==0 && depthFormat==VK_FORMAT_UNDEFINED`; also checked
  `test_standard_pbr_unlit.cpp`/`test_standard_pbr_shadow_gpu.cpp` (every
  `sig.colorCount` assignment in both sets it to 1, never 0).
- `src/rx_graph/tests/test_compile.cpp` lines 370-531 (compute-vs-graphics
  stage-resolution tests — storage buffer, texture input, history input,
  all exercised at compile-time), `src/rx_graph/tests/test_barriers.cpp`
  lines 80-360 (same classification at the barrier level, incl. the
  `compute-to-draw-buffer` TEST_CASE at line 347), `src/rx_graph/tests/
  test_execute_gpu.cpp` lines 895-1040 and 1520-1600 and 2200-2260 (every
  existing real-GPU compute-class pass test — all explicitly comment that
  they exercise graph MACHINERY only, "not any real compute dispatch",
  line 909).
- Vulkan-Headers (vendored, `.deps-cache/Vulkan-Headers-59090d379695625b/
  include/vulkan/vulkan_core.h:4442-4448`): `vkCreateComputePipelines`'s
  signature takes `VkPipelineCache pipelineCache` as its second parameter,
  structurally identical in position/type to `vkCreateGraphicsPipelines` —
  direct evidence the two calls share the same cache-object type by design
  (see row 8 and Verification health for the confidence tier on the
  broader "safe to share one cache instance across pipeline types" claim,
  which is standard Vulkan-spec knowledge not re-fetched from spec prose
  this session).
- `grep -rn "vkCreateComputePipelines\|VK_PIPELINE_BIND_POINT_COMPUTE\|vkCmdDispatch" src/` (excluding
  build/worktree dirs) — **zero matches**, confirming the ticket's own
  "Scope" claim directly.
- `grep -rn "addStorageImageOutput\|addStorageImageInput\|StorageImageOutput\|StorageImageInput\|VK_IMAGE_USAGE_STORAGE_BIT" src/rx_graph/`
  — **zero matches** (new finding this session; see row 7).

---

## The matrix

| # | Criterion (ticket text or discovered gap) | Verification method | Current code state (verified, cited) | Disposition | Proposed binding acceptance criterion |
|---|---|---|---|---|---|
| 1 | Slang compute entry-point **compilation** already stage-generic | Direct read, `compiler.cpp` full | **Already fully delivered, zero changes needed.** `compileImpl()` (`compiler.cpp:107-247`) takes an arbitrary `entryPointNames` vector, calls `findEntryPointByName()` per name, composes/links/codegens uniformly — nothing assumes exactly one vertex + one fragment entry point. `mapStage()` (`compiler.cpp:49-84`) already maps `SLANG_STAGE_COMPUTE -> VK_SHADER_STAGE_COMPUTE_BIT` (line 61-62) alongside every other Slang stage, including ray-tracing/mesh stages this project doesn't use yet. | N/A — already satisfied; verification-only | A new regression test (no `Compiler`/`Slang` code change) proves a compute-only `.slang` module with one `[shader("compute")]`/`[numthreads(x,y,z)]` entry point compiles via the EXISTING `compileFromSource`/`compileFromFile` path unmodified, and `CompileResult::entryPointCode[0].stage == VK_SHADER_STAGE_COMPUTE_BIT`. |
| 2 | Slang compute entry-point **reflection** already stage-generic, incl. storage-buffer AND storage-image resource kinds | Direct read, `reflection.cpp` full | **Already fully delivered, zero changes needed.** `reflect()` (`reflection.cpp:135-289`) is entry-point-count-generic (`allStages`, lines 166-170, ORs every entry point's mapped stage). `mapElementType()` (`reflection.cpp:69-131`) already maps a `RWTexture2D`-shaped (or any `readWrite`-access) resource to `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` (line 110) and `StructuredBuffer<T>`/`RWStructuredBuffer<T>` to `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` (line 117) — exactly the two resource kinds the ticket's own GPU test needs. `reflection_test.cpp` (219 lines, full read) has **zero compute-stage coverage today** (all 4 existing `TEST_CASE`s use `[shader("vertex")]`/`[shader("fragment")]` only) — confirming the ticket's own framing ("check for existing compute-stage coverage, expect none") exactly. | N/A — already satisfied; verification-only | A new regression test: a compute module declaring one `RWStructuredBuffer<uint>` and one `RWTexture2D<float4>` global reflects both correctly (types, set/binding, stage flags incl. `VK_SHADER_STAGE_COMPUTE_BIT`) via the EXISTING `reflect()` path unmodified. |
| 3 | `PipelineLayoutBuilder::build()` can build a layout for a single compute stage today | Direct read, `pipeline_layout.h`+`.cpp` full | **Already fully reusable, zero changes needed.** `build()` (`pipeline_layout.cpp:134-268`) is 100% stage-agnostic: `vkBinding.stageFlags = binding->stages` (line 196) and `vkRange.stageFlags = range.stages` (line 241) both come straight from the shader's reflected `ShaderLayoutInfo` — nothing hardcodes VERTEX/FRAGMENT anywhere in this file. The `externalSet0` (bindless) substitution path (lines 64-98, 169-171) validates by binding-number/descriptor-type only, never by stage, so it composes with a compute-only shader's reflection unmodified. | N/A — already satisfied; verification-only | A compute shader's reflected `ShaderLayoutInfo`, passed through the EXISTING `PipelineLayoutBuilder::build()` (with `externalSet0 = bindless.descriptorSetLayout()`), produces a valid `PipelineLayoutBundle` with no new code in `pipeline_layout.cpp`. |
| 4 | Bindless set 0 accessible from compute (the RT-compatibility rationale) | Direct read, `bindless.cpp:120-190` | **Already true today, zero changes needed.** All four of BindlessTable's set-0 `VkDescriptorSetLayoutBinding::stageFlags` (sampled image, sampler, storage buffer, comparison sampler — `bindless.cpp:142,147,152,159`) are `VK_SHADER_STAGE_ALL`, which per the Vulkan spec covers every stage a bound pipeline uses, including compute — not a graphics-only mask. `MaterialSystem::bindInstance()`'s own doc comment (`material_system.h:487-491`) confirms the binding CALL itself ("the caller binds the owning `rx::rhi::BindlessTable`'s own descriptor set once per frame/pass") is the caller's responsibility with `VK_PIPELINE_BIND_POINT_GRAPHICS` today — a compute consumer needs only the same call with `VK_PIPELINE_BIND_POINT_COMPUTE`, an established one-line pattern substitution, not new RHI surface. | N/A — already satisfied at the descriptor-layout level; verification-only | A discrimination-style regression asserts `stageFlags == VK_SHADER_STAGE_ALL` on every BindlessTable set-0 binding (so a future accidental narrowing would fail loudly), plus the new compute GPU test (row 9) itself binds set 0 at `VK_PIPELINE_BIND_POINT_COMPUTE` and successfully samples/reads a bindless resource from the compute shader as live proof. |
| 5 | Render-graph compute-class **barrier/scheduling** machinery is delivered (registry's own claim) | Direct read, `pass.h`+`render_graph.cpp`+`barriers.cpp`+`executor.cpp` full/targeted | **Accurate in effect, imprecise in the registry's own mechanism description — see Conflicts.** `hasAttachmentOutput()` (`render_graph.cpp:116-127`) classifies a pass Compute-class iff it declares no color/depth/history-output attachment; `computeClass = !pass.hasAttachmentOutput()` (`render_graph.cpp:641`) feeds `Pass::resolveAccess()` (lines 129-207), which resolves storage-buffer/texture-input/history-input stages to `VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT` for a Compute-class pass. `barriers.cpp` (full read) is itself stage-agnostic — it consumes only the already-resolved `VkPipelineStageFlags2`/`VkAccessFlags2`/`VkImageLayout` triple (`detail::applyAccess()`, lines 78-168) and never inspects `PassSignature` or compute-vs-graphics classification directly. **`PassSignature` (the struct the registry names) is a SEPARATE artifact**, populated in `executor.cpp:1406-1421` from the identical `hasAttachmentOutput()`-derived `colorPhysIdx`/`depthPhysIdx` locals, consumed ONLY by `MaterialSystem::getPipeline()`'s cache key (`material_system.cpp:1725,1939`) — barrier stage resolution never reads a `PassSignature` value at all. The registry's phrase "compute-class barrier stages derive from attachment-free signatures" is therefore true only in the sense that both derive from the same underlying fact; read literally (barriers computed FROM the `PassSignature` struct) it overstates the actual data flow. Real-GPU test scaffolding for compute-class passes already exists and is proven clean on real hardware (`test_execute_gpu.cpp:895-965,1535-1600`), using `vkCmdFillBuffer`/no-op as its GPU-write stand-in, not a real dispatch. | N/A for barrier correctness (no fix needed); **minor documentation correction only** | No code change. The Task 1 spec/ticket rewrite should correct the registry's own phrasing to "compute-class classification (and, derived from it, both barrier stages and `PassSignature`'s attachment-free shape)" so a future reader does not go looking for a `PassSignature`-consuming code path inside `barriers.cpp` that does not exist. |
| 6 | Compute PSO **creation** API — the ticket's real net-new surface | `grep` confirms absence; design required | Confirmed absent exactly as the ticket states: zero `vkCreateComputePipelines`/`VK_PIPELINE_BIND_POINT_COMPUTE`/`vkCmdDispatch` anywhere in `src/`. This is genuinely new code, not a verification task. | consume-now — the task's actual core deliverable | A new PSO-creation path (see row 8 for WHERE) builds a `VkComputePipelineCreateInfo` from one compiled+reflected compute `VkShaderModule` + a `PipelineLayoutBundle` (row 3), calls `vkCreateComputePipelines`, and caches the result keyed at minimum by (shader content hash, pipeline-layout shape) — no `PassSignature`/attachment-shape axis is meaningful for a compute PSO (a compute pipeline has no attachment state to vary over), so the cache key is structurally SIMPLER than `MaterialSystem`'s `PipelineKey`, not a reuse of it. |
| 7 | **NEW FINDING, not named by the ticket or the existing plan file-list: `rx_graph` has ZERO storage-IMAGE (UAV) declaration kind** — directly blocks the ticket's own required "storage-buffer AND a storage-image write" GPU test | Direct read, `resources.h`+`pass.h` full, `render_graph.cpp:645-710` full (exhaustive `switch(decl.kind)`), `grep` confirmation | **Confirmed absent, load-bearing gap.** `Pass::AccessKind` (`pass.h:291-305`) has exactly 6 values: `ColorOutput, DepthStencilOutput, TextureInput, StorageBufferOutput, StorageBufferInput, HistoryInput, HistoryOutput` — no storage-image kind. `Pass` (`pass.h`) exposes `addStorageBufferOutput()`/`addStorageBufferInput()` for buffers but has no `addStorageImageOutput()`/`addStorageImageInput()` at all. `PhysicalResource::imageUsage`'s own doc comment (`resources.h:133-141`) enumerates only `COLOR_ATTACHMENT_BIT`/`DEPTH_STENCIL_ATTACHMENT_BIT`/`SAMPLED_BIT` as derivable usage bits — `VK_IMAGE_USAGE_STORAGE_BIT` is never mentioned. The compile-time `switch (decl.kind)` in `render_graph.cpp:669-709` is EXHAUSTIVE over the current 6-value enum (no `default:` case) and unions `imageUsage` for `ColorOutput`/`DepthStencilOutput`/`TextureInput`/`HistoryInput`/`HistoryOutput` only — `grep -rn "VK_IMAGE_USAGE_STORAGE_BIT" src/rx_graph/` returns zero hits anywhere. `addColorOutput()` is not a workaround: it forces `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` and a `vkCmdBeginRendering` scope (`executor.cpp` `isGraphicsPass` branch) — wrong layout and wrong pass classification for a compute UAV write. `TransientPool::acquireImage()`/`acquireHistory()` (`transient_pool.h:222,259`) both take a plain `VkImageUsageFlags usage` parameter fed verbatim from `physical.imageUsage` (`executor.cpp:1100,1135`), so the fix is narrowly scoped: no pool-level change is needed once `imageUsage` carries the new bit. | **consume-now — new gap, not covered by the ticket's own file list** (`plan.md:186-192` lists `src/rx_graph` only as "compute pass execution wiring if any gap remains", implying a minor/possible gap; this is a real, certain one) | New `Pass::AccessKind::StorageImageOutput`/`StorageImageInput` values; new `Pass::addStorageImageOutput(name, ImageDesc)`/`addStorageImageInput(name)` builder methods (mirroring `addStorageBufferOutput`/`Input`'s existing shape); a new `resolveAccess()` row per kind (`VK_IMAGE_LAYOUT_GENERAL`, `VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT`\|`READ_BIT` as appropriate, stage split on `computeClass` exactly like the existing `StorageBufferOutput`/`Input`/`TextureInput` rows); union `VK_IMAGE_USAGE_STORAGE_BIT` into `PhysicalResource::imageUsage` for both new kinds in the `render_graph.cpp` switch. Test: the new GPU test (row 9) IS this feature's own acceptance test — a compute pass writes a `RWTexture2D` through the new declaration, a downstream pass samples/reads it, values match, barriers are graph-derived (not hand-written), zero validation errors. |
| 8 | **Architectural placement** — MaterialSystem-owned compute-pipeline resolution vs. a parallel `ComputePipeline`/cache facility (ticket's own explicitly-deferred choice) | Direct read + layering analysis | `MaterialSystem` is deeply graphics/material-instance-specific by construction, not merely "vertex+fragment by accident": it hardcodes a 2-stage `VkPipelineShaderStageCreateInfo` array (`material_system.cpp:1950-1964`), derives fixed-function blend/cull/depth state from a material's `alphaMode`/`doubleSided` (lines 1983-2048, D28), fixes one vertex-input layout (`makeVertexInputState()`), and owns the **only** `VkPipelineCache` in this codebase privately (`material_system.cpp:1354-1465`, `Impl::pipelineCache`, no accessor anywhere in `material_system.h`). Layering fact: `rx_rhi_vk` sits structurally BELOW `rx_material` (MaterialSystem consumes `rx_rhi_vk`'s `PipelineLayoutBuilder`, never the reverse); this plan's own named compute consumers — Task 9 (IBL bake, `rx_scene`/`rx_asset`-orchestrated per `plan.md:382`) and Task 14 (froxel clustering, `rx_scene`-orchestrated per `plan.md:496`) — are architectural peers or siblings of `rx_material`, not children of it, and have no "material instance" to bind (`bindInstance()`'s own set-1 material-param-arena binding, `material_system.h:477-499`, is meaningless for a general compute kernel like an IBL prefilter pass). | **open-for-coordinator (explicitly deferred by the ticket) — this matrix's decisive recommendation: a new, parallel `rx::rhi::ComputePipeline`/cache facility living in `rx_rhi_vk`, sibling to `MaterialSystem`, not nested inside it.** See Open Questions #1 for the full rationale. | Whichever the coordinator rules, the binding criterion is: compute-pipeline creation/caching lives at a layer every Phase-5 compute consumer (Task 9, Task 14, and Task 2's own tests) can reach WITHOUT depending on `rx_material`'s graphics-specific machinery — reusing `PipelineLayoutBuilder::build()` (row 3, already layer-appropriate in `rx_rhi_vk`) regardless of which side wins. |
| 9 | Pipeline-cache reuse for compute PSOs — ticket's literal text: "cached in the existing VkPipelineCache" | Direct read + Vulkan-Headers signature check | **The ticket's literal claim is false as written; the underlying intent (share ONE cache mechanism, not reinvent persistence) is sound.** No shared/accessible `VkPipelineCache` exists today outside `MaterialSystem`'s private instance (row 8) — there is no "the existing VkPipelineCache" a new module can reach. Separately, `vkCreateComputePipelines` and `vkCreateGraphicsPipelines` (Vulkan-Headers `vulkan_core.h:4400-4448`) take the identical `VkPipelineCache` handle type as their second parameter — a real Vulkan design fact (not this codebase's own choice) that ONE cache instance is legitimately usable across both pipeline types if the two creation paths shared an instance; this codebase's own `MaterialSystem` cache is simply not reachable from outside `rx_material` today, an encapsulation fact, not a Vulkan-spec limitation. | consume-now, corrected (see Conflicts) | Under the row-8 recommended architecture: the new `rx_rhi_vk` compute-pipeline facility owns its OWN `VkPipelineCache` instance, using the SAME proven disk-persistence pattern `MaterialSystem::create()`/`~MaterialSystem()` already establishes (load-if-present at construction, `vkGetPipelineCacheData` + write-to-disk at teardown, malformed-cache-is-a-warning-not-fatal per `material_system.cpp:1396-1401`) — pattern reuse, not reinvention, but a separate cache instance/file, never literally "the existing" one. The ticket's acceptance text should be corrected to "cached in a VkPipelineCache using the same persistence discipline as MaterialSystem's" rather than implying one shared handle. |
| 10 | GPU test: storage-buffer AND storage-image write, downstream read, exact VALUES, graph-derived barriers, zero validation errors, lavapipe + real driver | Design (no existing test to extend directly) + existing infra to reuse | No existing test does this (row 6/7 confirm absence). Reusable infrastructure IS proven: `device.cpp:223` already enables `synchronization2`; the existing GPU-test-fixture convention already runs with validation active (`bindless_test.cpp`, `clear_color_test.cpp`, and every `test_execute_gpu.cpp` fixture via `makeFixture()`); `test_execute_gpu.cpp:895-965`'s "compute-to-draw-buffer" shape (a `QueueClass::AsyncCompute` pass writing a storage buffer, consumed by a downstream graphics pass, readback-verified) is the direct template to extend — it only needs (a) row 7's new storage-image declaration added to the same pattern, and (b) the pass callback's `setSideEffect()`/no-op `setExecute()` stand-in replaced with a REAL `vkCmdBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, ...)` + `vkCmdBindDescriptorSets` + `vkCmdDispatch` sequence. | consume-now — this row IS the ticket's primary acceptance gate | Compute pass 0 dispatches a shader writing a deterministic, hand-computable pattern (e.g. `buf[i] = i*7+3`; `image[xy] = float4(xy.x/W, xy.y/H, 1, 1)`) to a `RWStructuredBuffer` (existing `addStorageBufferOutput`) AND a `RWTexture2D` (row 7's new `addStorageImageOutput`); a downstream pass (compute or graphics) reads both via ordinary input declarations, barriers entirely graph-derived (never hand-written `vkCmdPipelineBarrier2` in the test); CPU readback asserts every element/texel against the hand-computed expected value (exact equality for the integer buffer; exact-bit or tight documented tolerance for the float image, per format). Run on BOTH lavapipe and the real (NVIDIA) driver per the phase's standing "lavapipe-only is not verification" constraint (`plan.md:65-73`), each run's driver labeled in the report, zero validation messages on either. |
| 11 | Attachment-free `PassSignature` accepted end-to-end; "the old rejection path's test updated, not deleted" | Direct read, `material_system.cpp:1921-1975` + exhaustive test-suite grep | `getPipeline()` (`material_system.cpp:1933-1937`) unconditionally throws `std::runtime_error` for `req.pass.colorCount == 0 && req.pass.depthFormat == VK_FORMAT_UNDEFINED`, tagged "[Task 5 ambiguity resolution] no Phase 3 use case for a graphics pipeline with neither a color nor a depth attachment" — a correct, deliberate guard: `getPipeline()` only ever builds a hardcoded 2-stage VERTEX+FRAGMENT graphics pipeline (lines 1950-1964); a graphics pipeline genuinely has no valid use with zero attachments, Vulkan-spec-wise, independent of Phase 5. **Exhaustively grepped `test_material_system.cpp` (and `test_standard_pbr_unlit.cpp`/`test_standard_pbr_shadow_gpu.cpp`) for any test exercising this exact rejection: zero hits.** Every `PassSignature`/`sig.colorCount` construction in the test suite sets `colorCount = 1` (or higher); none constructs the degenerate all-zero case and asserts the throw. `PassContext::passSignature()`'s own doc comment (`executor.h:319-321`) independently confirms this is a real, reachable rejection path for a genuinely "bare" pass, just one no test currently drives. | consume-now, corrected (see Conflicts) — **this is a pre-existing test gap the gate discovered, not a test due for an update** | Under the row-8 recommended architecture (parallel `ComputePipeline` facility): `getPipeline()`'s rejection is **preserved unchanged**, since it is still correctly rejecting a nonsensical GRAPHICS-pipeline request — the "lifting" the ticket names happens at the render-graph/consumer level (an attachment-free pass becomes routable to the NEW compute-capable path), not by weakening this check. ADD (the grep above shows none exists to update) a `test_material_system.cpp` case asserting `getPipeline()` still throws `std::runtime_error` for `colorCount==0 && depthFormat==VK_FORMAT_UNDEFINED`, closing the coverage gap this session found, independent of Task 2's own new-facility work. |
| 12 | Zero validation errors with sync validation, both drivers — infra reuse | Direct read, `device.cpp:223` + existing fixture pattern | Infrastructure already proven and reusable: `synchronization2` is already enabled at device-creation time; every existing GPU-test fixture (`makeFixture()` pattern across `test_execute_gpu.cpp`, `bindless_test.cpp`, `clear_color_test.cpp`) already runs with a validation layer active including sync validation. No new harness needed. | N/A — infrastructure already satisfied; only new test CONTENT (row 10) is net-new | Row 10's new compute GPU test runs clean (zero validation messages) on both lavapipe and the real driver, using the EXISTING fixture/validation setup unmodified. |

---

## Conflicts

1. **Row 5**: the registry's own text (`toolchain-platform-rhi-design.md:
   248-250`) states "compute-class barrier stages derive from
   attachment-free signatures" — verified to describe the OUTCOME
   accurately but the MECHANISM imprecisely. `barriers.cpp` (full read)
   never reads a `PassSignature` value; barrier-stage resolution
   (`Pass::resolveAccess`, `render_graph.cpp:129-207`) and `PassSignature`
   construction (`executor.cpp:1406-1421`) are two independent
   re-derivations of the SAME upstream fact (`Pass::hasAttachmentOutput()
   == false`), computed in different files for different consumers
   (barrier machinery vs. `MaterialSystem`'s pipeline cache key). Not a
   functional defect — both derivations are correct and mutually
   consistent by construction — but a future reader taking the registry
   sentence literally would go looking for a `PassSignature`-consuming
   code path inside `barriers.cpp`/`Executor` that does not exist.
2. **Row 9**: the ticket's acceptance sketch states compute PSOs are
   "cached in the existing VkPipelineCache" — verified FALSE as a literal
   claim: no shared/accessible `VkPipelineCache` exists outside
   `MaterialSystem`'s own private instance. Resolved by recommending a
   second, independent cache instance using the same proven load/persist
   pattern (row 8/9), not a literally-shared handle.
3. **Row 11**: the ticket's acceptance sketch states "the old rejection
   path's test updated, not deleted" — verified FALSE: no such test exists
   today (exhaustive grep, zero hits). Not a contradiction of the ticket's
   INTENT (the rejection behavior should indeed be preserved and tested,
   which this matrix's row 8/11 recommendation delivers), only of its
   factual premise that a test already does so — resolved by re-framing
   the criterion as "add," not "update."

## New gaps

- **Storage-image (UAV) Pass-API surface in `rx_graph` (row 7).** Not named
  anywhere in the ticket text, the plan's Task 2 file list, or the
  registry entry — all three describe this ticket as verification-plus-PSO
  -creation with "compute pass execution wiring if any gap remains" as a
  hedge, not a certainty. This session's exhaustive read of
  `resources.h`/`pass.h`/`render_graph.cpp`'s compile-time switch confirms
  the gap is real and certain, not hypothetical: the render graph cannot
  today express a compute shader's `RWTexture2D` write at all, through any
  existing declaration kind. Proposed phase fit: **THIS ticket (P5 T02),
  not deferred** — the ticket's own required GPU test (row 10, ticket's
  own acceptance sketch) is impossible to write without it, so it cannot
  be scoped out without also dropping half the ticket's primary gate.
- **`getPipeline()`'s attachment-free rejection has zero existing test
  coverage (row 11).** A pre-existing gap (not introduced by this ticket),
  surfaced because this gate specifically went looking for the test the
  ticket assumes exists. Proposed phase fit: closed in THIS ticket
  alongside the new compute-facility work, since the same
  `PassSignature`/`getPipeline()` code this ticket's acceptance criteria
  already touch is exactly where the missing test belongs.

## Open Questions

1. **Architectural placement (rows 8/9/11): parallel `rx::rhi::ComputePipeline`
   facility in `rx_rhi_vk` vs. a compute-capable branch inside
   `MaterialSystem`.** **Recommendation: parallel facility in `rx_rhi_vk`**,
   reusing `PipelineLayoutBuilder::build()` and owning its own
   `VkPipelineCache` (same disk-persistence pattern as `MaterialSystem`,
   separate file). Rationale: (a) layering — Task 9/14's actual compute
   consumers sit at or above `rx_scene`/`rx_asset`, architectural siblings
   of `rx_material`, not children of it; routing them through
   `MaterialSystem` would either invert the dependency graph or force
   every non-material compute kernel (an IBL prefilter pass has no
   "material") through a class whose entire design — fixed-function
   blend/cull/depth-from-alphaMode, one hardcoded vertex-input layout,
   material-instance param-arena binding — is graphics-specific by
   construction; (b) `getPipeline()`'s own hardcoded 2-stage shader-stage
   array (row 11) would need a real conditional split to grow a compute
   branch, at which point a sibling class is barely more code and is
   architecturally cleaner. This is decisive, not a survey: the
   coordinator should rule it explicitly since it determines rows 9/11's
   exact shape and the new facility's location in Task 2's file list.
2. **Does `MaterialSystem::getPipeline()`'s attachment-free rejection
   change at ALL?** **Recommendation: no** (row 11) — under the
   recommended architecture it stays byte-identical; the ticket's "lifting
   the rejection" language should be corrected in the rewritten acceptance
   criteria so an implementer does not weaken a still-correct graphics-only
   guard. The "lifting" is entirely about a NEW code path becoming
   reachable for attachment-free passes, never about relaxing this one.
3. **Storage-image Pass API shape (row 7): mirror `StorageBufferOutput`/
   `Input` exactly, or something richer (e.g. explicit mip/layer
   addressing)?** **Recommendation: mirror the existing
   `StorageBufferOutput`/`StorageBufferInput` shape exactly** —
   `addStorageImageOutput(name, ImageDesc)`/`addStorageImageInput(name)`,
   same `computeClass`-split stage resolution `TextureInput`/
   `StorageBufferOutput`/`Input` already use, `VK_IMAGE_LAYOUT_GENERAL`
   fixed (the only legal generic UAV layout), no mip/layer-subrange
   addressing in this task. Rationale: this ticket's own required GPU test
   (row 10) only needs a single full-image UAV write; a richer API
   (partial mip/layer views) is unrequested scope with no named consumer
   in this plan and should wait for a task that actually needs it (mirrors
   this codebase's own repeated pattern of shipping the minimal shape a
   named consumer needs, e.g. D9's "no defragmentation" scoping in the
   Phase-4 GeometryPool matrix).
4. **Should the compute-pipeline cache key include ANYTHING resembling
   `PassSignature`?** **Recommendation: no** — a compute pipeline has no
   attachment state (no `VkPipelineRenderingCreateInfo`, no blend/depth/
   rasterization state) to vary over, so the entire reason `PassSignature`
   exists (keying `MaterialSystem`'s graphics-pipeline cache on attachment
   SHAPE) does not apply; the new facility's cache key should be just
   (shader content hash, pipeline-layout shape) — simpler than
   `MaterialSystem::PipelineKey`, not a reuse of it. Flagged because the
   ticket's Files list groups this ticket's `rx_material` work under
   "compute-capable pipeline resolution" language that could be misread as
   implying `PassSignature` involvement for compute too.

## Verification health

**Verified first-hand this session, direct full or targeted-complete
reads, exact file:line citations:** `pass_signature.h` (full, 102 lines),
`pass.h` (full, 369 lines), `resources.h` (full, 172 lines), `barriers.cpp`
(full, 256 lines), `compiler.cpp` (full, 351 lines), `reflection.cpp` (full,
291 lines), `reflection_test.cpp` (full, 219 lines), `pipeline_layout.h`
(full, 163 lines), `pipeline_layout.cpp` (full, 270 lines),
`render_graph.cpp` (targeted-complete: lines 100-320, 630-715),
`executor.cpp` (targeted: lines 1370-1440), `executor.h` (targeted: lines
295-340), `bindless.cpp` (targeted: lines 120-190), `material_system.cpp`
(targeted: lines 100-140, 1900-2090, 1350-1560), `material_system.h`
(targeted: lines 460-500), `transient_pool.h` (signature-level), plus every
grep cited above run directly against this session's checkout (not assumed
from the ticket/registry text). The storage-image gap (row 7) and the
missing-rejection-test finding (row 11) were independently re-derived, not
copied from any prior artifact.

**Verified via structural (not prose) evidence:** the `VkPipelineCache`
cross-pipeline-type sharing claim (row 9) rests on the vendored Vulkan
header's own function signatures (`vkCreateComputePipelines`/
`vkCreateGraphicsPipelines` both taking `VkPipelineCache` in the identical
parameter position) plus standard, well-established Vulkan-spec knowledge
that pipeline cache objects are pipeline-type-agnostic — this general spec
fact was not re-fetched from spec prose this session (no vendored spec
text exists in this repo's dependency tree to fetch from), consistent with
how this codebase's own comments treat other uncontested core-API facts
(e.g. the 128-byte push-constant floor citation in `pipeline_layout.cpp`).
Confidence: high for the structural signature fact (directly read), high-
but-not-independently-re-fetched for the broader spec guarantee.

**Not verified / left to the implementer:** the EXACT public API shape of
the new compute-pipeline facility (class/method names) — this matrix
identifies WHERE it should live (row 8), WHAT it must reuse (rows 3, 9),
and WHAT new `rx_graph` surface it depends on (row 7), not its literal
interface, which is implementation-phase design work per this gate's own
scope. Storage-image format/`VkFormatFeatureFlags` STORAGE_IMAGE support
per format was not audited — a real Vulkan constraint (not every format a
`TransientPool` might allocate is guaranteed storage-capable) flagged here
for the implementer to check against the specific format the new GPU test
picks, not resolved by this gate.
