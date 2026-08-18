# Phase 4: Scene & Assets — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Three stages — Foundations & Debt, Asset Pipeline, Scene & Culling — delivering samples 07_stress / 08_gltf_viewer / 09_scene and release v0.4.0-phase4.

**Spec:** `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md` (decisions D1-D23, gaps G1-G15). Research: `.superpowers/sdd/2026-08-11-phase4-scene-assets/research-p4-{assets,threading,scene,present}.md`. Board: issues map task-per-card, labels `stage-0/1/2` (https://github.com/users/ywadi/projects/2).

**Architecture:** New libraries `src/rx_task` (enkiTS wrapper), `src/rx_asset` (import + GeometryPool + registry), `src/rx_scene` (proxies + DrawListBuilder), `src/rx_debug_ui` (ImGui). New third-party (all via DepCache/vendoring with pinned tags verified at adoption): enkiTS, Tracy client, fastgltf, libktx (KTX-Software), MikkTSpace. Samples 07/08/09.

**Tech Stack additions:** enkiTS (zlib), Tracy client (BSD-3), fastgltf (MIT), libktx (Apache-2.0), MikkTSpace (zlib-class — verify at vendoring), Dear ImGui v1.92.x (MIT). Everything else existing.

## Global Constraints

- All Phase 1-3 global constraints remain (attribution ban; production grade; sync2/dynamic-rendering only; zero validation errors with sync validation active; both presets build; TDD; warm-up pattern for GPU test binaries; established style per directory).
- CLAUDE.md performance policy binds every task: measured claims only (Tracy/counters), fast-path-as-default designs, counters gated in CI (D18), numbers published per stage.
- Threading contract (D5): GPU-object mutation main-thread-only; workers never touch BindlessTable/Uploader/MaterialSystem/DeletionQueue; violations are review-blocking. Every new public header states its thread affinity in one line.
- New dependencies: pin exact tags, record license + tag in the vendoring commit message, verify windows-cross-zig builds in the SAME task that adopts the dependency.
- Sample numbering: 07_stress, 08_gltf_viewer, 09_scene. Each ships in `tools/package_samples.sh` + CI gates in the task that creates it.
- Board discipline: when a task completes, the coordinator moves its card; implementers do not touch the board.

---

## STAGE 0 — Foundations & Debt

### Task 1: Render-graph history resources + PassSignature bounds check

**Files:** Modify `src/rx_graph/{render_graph.cpp,executor.cpp,transient_pool.{h,cpp}}`, `src/rx_graph/include/rx_graph/{render_graph.h,pass.h,resources.h}`; tests in both rx_graph targets.
**Interfaces (produces):**
```cpp
// Pass declaration additions:
Pass& addHistoryInput(std::string_view name);   // sampled read of the resource's PREVIOUS-frame contents
Pass& setHistoryOutput(std::string_view name, const AttachmentDesc& desc); // persistent (non-discard) written image
```
Semantics per spec item seed-15: history resources live in a pinned pool (never recycled/aliased/swapped between logical resources), ping-pong internally (read slot = last frame's write slot) under frames-in-flight; first-ever use initializes via UNDEFINED-discard + documented "history invalid on frame 0" contract (pass callback can query `PassContext::historyValid(name)`); subsequent frames load-preserve. Compile-time: history inputs read SHADER_READ_ONLY_OPTIMAL with FRAGMENT|COMPUTE stages per pass kind; barrier state machine initialized from tracked last-frame layout instead of UNDEFINED. Executor: pinned entries track lastFrameFinalStages/Access exactly like pooled ones.
**Also in this task (final-review carry):** `compile()` throws when a pass declares more color outputs than `PassSignature::kMaxColorAttachments` (8) — loud, named error + device-free test.
**Steps:** device-free tests (declaration/compile semantics, bounds check) → GPU test: pass A writes frame-N pattern to history output, pass B reads history input and writes readback target; assert frame N+1 readback contains frame N's pattern, frame 1 handles invalid-history branch; zero validation errors (sync validation) → implement → both presets → commit `feat: add render-graph history resources and color-attachment bounds check`.

### Task 2: enkiTS adoption (`rx_task`) + threading contract doc

**Files:** Create `src/rx_task/{CMakeLists.txt,include/rx_task/scheduler.h,scheduler.cpp,tests/...}`, `docs/threading.md`; modify `cmake` dep wiring for enkiTS (pinned tag, DepCache), root CMakeLists.
**Interfaces (produces):**
```cpp
namespace rx::task {
class Scheduler {  // owns enki::TaskScheduler; one per app; main thread participates
 public:
  static std::unique_ptr<Scheduler> create(uint32_t workerCount /*0 = hw-1*/);
  void parallelFor(uint32_t itemCount, std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn); // blocking fan-out, AUTO grain (parallelism-default: no caller knobs)
  void parallelFor(uint32_t itemCount, uint32_t grainSize, std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn); // explicit grain = measurement affordance only
  void runOnIoThread(std::function<void()> fn);         // pinned IO thread, FIFO
  void postToMain(std::function<void()> fn);            // queued; drained by pumpMain()
  void pumpMain();                                      // main-thread drain point (frame loop calls once per frame)
  uint32_t workerCount() const;                          // includes main participation semantics documented
};}
```
`docs/threading.md`: the D5 contract (main-thread-only list, worker-allowed list, handoff pattern, per-thread command-pool rule forward-referencing Task 7). Thread-affinity one-liners added to bindless.h/upload.h/material_system.h/deletion_queue.h headers (doc-only edits).
**Steps:** vendor enkiTS (pin tag; license recorded) → tests: parallelFor sums ranges exactly once (counters per worker), postToMain executes on main thread id, IO-thread ordering FIFO, nested parallelFor safe → both presets (windows-gnu build verified) → commit.

### Task 3: Tracy integration

**Files:** Create `src/rx_core/include/rx_core/profile.h` (RX_ZONE macros wrapping Tracy, no-op when disabled); vendor Tracy client (pinned tag, TRACY_ENABLE option, ON in dev presets); modify frame-path files to add zones (FrameSync acquire/present, Executor::execute + per-pass zones using pass names, MaterialSystem::getPipeline/loadMaterial, Uploader submits); GPU ctx: `src/rx_rhi_vk/tracy_gpu.{h,cpp}` — TracyVkContextCalibrated when VK_EXT_calibrated_timestamps present else TracyVkContext; collect per frame.
**Constraints:** zones are cheap macros — no allocations, no behavior change when disconnected; GPU ctx guarded by extension query (lavapipe support empirically checked in-task and documented either way); windows-cross build verified.
**Steps:** vendor+option wiring → zone macros + placements → GPU ctx guarded → verify: connect Tracy locally, capture a sample-05 run, screenshot/txt evidence in report; suite green both presets → commit.

### Task 4: Material texture sampling wiring (seed 10 / carried)

**Files:** Modify `shaders/material/material.slang` + `forward_entry.slang` (bindless texture array + sampler array access for materials: `float4 rx_sampleTexture(uint index, float2 uv)` helper), `src/rx_material/material_system.cpp` (`reflectMaterialLayout()` allow-list accepts the material-side bindless references), tests (+`tests/data/test_textured_sample.slang`).
**Acceptance (the carried bar):** a `createTexture2D`-created texture bound via `setTexture` VISIBLY changes rendered output — GPU test renders a quad with a 2×2 texture through the public API and asserts the four quadrant colors; hot-reload of a textured material keeps working. GUID regen on any ABI-visible change per documented policy.
**Steps:** failing GPU test → implement → suite green both presets, zero validation errors → commit.

### Task 5: Public log sink (seed 13, card #17)

**Files:** Modify `src/rx_material/include/rx_material/rx_api.h` (C types: `RxLogSeverity` enum, `RxLogCallback` fn-ptr typedef, `extern "C" RxResult rxSetLogCallback(RxLogCallback cb, void* userData)`), `api_impl.cpp` + new spdlog forwarding sink `src/rx_core/log_forward_sink.{h,cpp}`; tests.
**Rules:** callback receives (severity, category cstring, message cstring, userData); invocation wrapped in catch-all (a throwing callback is swallowed + disabled with one console warning); may fire from any thread (documented); nullptr cb restores console-only; header self-containment test extended.
**Steps:** device-free tests (install/uninstall, capture of a logged message incl. from a worker thread via rx_task, throwing-callback disable path) → implement → both presets → commit.

### Task 6: Present-mode control (seed 1)

**Files:** Modify `src/rx_rhi_vk/device.{h,cpp}` (`Device::setPresentMode(PresentMode)` — enum VsyncOn/VsyncOff; recreates swapchain via existing recreate path with vkb `set_desired_present_mode` ladder: VsyncOn=FIFO; VsyncOff=MAILBOX→IMMEDIATE→FIFO-with-warning [R:present]; explicit default = current behavior made explicit as VsyncOn? **No** — explicit default FIFO for samples without the flag, MAILBOX-preference removed so behavior is *chosen*, documented); all six samples gain `--vsync on|off` (default on) parsed like `--validate`; `samples/README.md` rows.
**Steps:** device-free arg-parse tests where samples have them; manual+gate verification: headless unaffected; present-mode toggle exercised in sample 07 (Task 7 consumes); recreate-on-toggle validated (resize test pattern reused); zero validation errors → commit.

### Task 7: Parallel command recording + sample 07_stress

**Files:** Modify `src/rx_graph/include/rx_graph/pass.h` + `executor.cpp` (opt-in parallel recording per D4): 
```cpp
// No opt-in flag and no caller-chosen count: providing a chunked callback IS the parallel path
// (executor derives chunk count from the scheduler; grain scaling handles small workloads).
Pass& setExecuteChunked(std::function<void(PassContext&, uint32_t chunkIndex, uint32_t chunkCount)> fn);
// PassContext gains: VkCommandBuffer chunkCommandBuffer() — the secondary this chunk records into (workers)
```
Executor: per-thread × per-frames-in-flight command pools (created lazily per scheduler worker count); for parallel passes: begin rendering with SECONDARY_COMMAND_BUFFERS contents, secondaries begun with VkCommandBufferInheritanceRenderingInfo matching the pass's attachment formats/samples [R:threading], chunks fanned out via rx_task parallelFor, vkCmdExecuteCommands in chunk order, pools reset per frame slot. Whole-pass-callback passes byte-identical to today (they are the hand-written simple case, not a disabled mode). **Samples 05 and 06 migrate to chunked callbacks in this task** (user-directed): auto-grain collapses them to one chunk (no perf change, headless pixel gates must stay byte-identical) but their CI gates then exercise the parallel recording path on every commit. Samples 01-04 stay pre-graph by design — they document the layers below the executor.
Create `samples/07_stress/` + `shaders/stress/*.slang`: procedural instanced field (default 30,000 draws — cubes/spheres mix, per-instance transform+color via bindless arena, 4 pipeline/material variations to make sorting/state non-trivial), flags: `--draws N --threads N --vsync on|off --validate` (threads default = scheduler default, i.e. parallel recording ON; `--threads 1` is the A/B baseline), forward+tonemap through the graph with the forward pass parallel-recorded; ImGui NOT yet (Stage 2) — stats to stdout each second + Tracy zones; headless gate: fixed 3 frames, counter assertions (exact draws submitted, chunk count = threads, pool allocations within budget) + tolerance probe on 4 analytic pixels; **CI counter gate** (D18) + wall-clock printed and uploaded as artifact `stress-numbers.txt`; report publishes single-vs-multi-thread record timings (Tracy evidence) on the dev machine.
**Steps:** TDD gate → implement executor path → sample → measurements → packaging/CI wiring → commit(s).

### Task 8: Ledgered-minors cleanup batch (Haiku)

**Files/items (each fully specified, mechanical):**
1. `rx_material` corrupt-pipeline-cache-content regression test (write garbage bytes file → create succeeds with warning + fresh cache).
2. `MaterialSystem::layoutInfo()` doc: reference invalidated by later loadMaterial (HandlePool reallocation) — copy the wording style from Impl::materialHandles.
3. `rx_graph` dead-cyclic-subgraph doc polish (render_graph.cpp culling comment + one header line).
4. `rx_shader::reflect()`: expose storage-buffer element stride in ShaderLayoutInfo (plumb from Slang type layout; add unit test vs a known struct) AND upgrade sample 05's ObjectTransform static_assert to compare against the reflected stride at material/pipeline build (kills the hand-computed constant).
5. `ParamArena::writeAndAllocate`: advance byte cursor only after successful descriptor allocate (removes documented waste path; update its test).
6. `.github/workflows/ci.yml`: echo installed vulkan-validationlayers version in the test step log (visibility for the guard-fragility watch-item); add a comment documenting the pinned-version upgrade procedure.
**Steps:** per item: test-first where testable → fix → suite green both presets → single commit `chore: clear phase 1-3 ledgered minors`.

### Stage 0 exit gate: Foundation audit (Fable-model, user-mandated)

After Tasks 1-8 close and before Stage 0 is declared complete, a
**Fable-model audit agent** performs an adversarial audit of the ENTIRE
existing foundation — everything Stage 1+ builds on: rx_core, rx_platform,
rx_rhi_vk, rx_shader, rx_graph, rx_material, rx_task, shaders/, samples,
build system, CI, and the binding docs (threading contract, ABI rules,
performance policy). Scope: cross-subsystem seams and lifetimes,
concurrency contracts vs. actual implementations, synchronization
correctness beyond what per-task reviews could see, ABI discipline,
test-coverage honesty (what the gates actually prove vs. claim),
docs-vs-reality drift, and adherence to every CLAUDE.md policy. The
auditor probes empirically (builds, tests, targeted instrumentation in
scratch), not just by reading. **Every finding is triaged by the
coordinator and closed via fix rounds (or explicitly ruled + recorded)
before Stage 0 exits — the stage is NOT complete until the audit report
and its closure record are in the ledger.**

---

## STAGE 1 — Asset Pipeline

### Task 9 (PRIMARY GATE): Ticket completeness research & hardening

**User-mandated 2026-08-11.** Before ANY Stage 1/2 implementation, every
Phase 4 ticket is deepened from its current vague form into a detailed,
production-grade specification measured against what first-tier renderers
(Filament, bgfx, Godot, the glTF 2.0 spec in full, Unreal/Unity feature
expectations) actually require. The feature-gap audit found MISSING
capabilities; this gate hardens the capabilities that ARE planned but
under-specified. It is a research + coordinator-authoring task, NOT
implementation.

**Process (research gathers, coordinator authors — per the standing
mandate):**
1. Fable/Sonnet research agents produce, per ticket, a completeness
   matrix: [required feature] × [first-tier-renderer precedent] ×
   [consume-now / preserve-for-later / log-don't-drop / genuinely-N/A for
   Phase 4] × [does our chosen library actually support it, cited]. Every
   claim cited; the library's real capability (e.g. fastgltf's actual
   per-extension support, and which need external decode libs) verified,
   not assumed.
2. The coordinator rewrites each ticket body + the corresponding plan
   task with concrete, exhaustive acceptance criteria from that matrix —
   nothing vague survives.
3. Any newly-surfaced missing capability is registered (feature-gap
   register) with a phase fit, same as the prior audits.

**Coverage bar — all Phase 4 tickets, Stage 1 blocking, Stage 2 hardened
in the same pass:**
- **glTF import (#2)** is the worked example of the depth required. The
  rule (established 2026-08-11): a renderer need not *render* every glTF
  feature in Phase 4, but it MUST (a) **decode** whatever is needed to
  open the file at all — **compression is non-negotiable: EXT_meshopt_
  compression (gltfpack's output, the de-facto shipping format;
  meshoptimizer is the committed decode library — gate correction
  2026-08-18: NOT yet vendored, Task 13 budgets it), KHR_mesh_quantization,
  KHR_draco_mesh_compression** — a file that won't load is not "partial
  import," it is a broken importer; (b) **preserve** what later phases
  consume — **animation channels/samplers and morph targets**, same
  seed-14 economics as skinning/lights/cameras; (c) **log, never silently
  drop** everything else — KHR_texture_transform, image-source variants
  (external-URI / data-URI / .glb-bufferView), `extras` application JSON
  (game devs' own data), non-triangle primitive modes, and every
  KHR_materials_* extension beyond core. The hardened #2 enumerates each
  with its disposition.
- Every other ticket (GeometryPool, KTX2, async import, StandardPBR,
  window state, scene, draw lists/culling, layers, input, ImGui, shadows,
  sample 09) gets the same treatment against its domain's first-tier bar
  (e.g. StandardPBR vs the full glTF metallic-roughness + the interim
  ambient FG1 + which KHR_materials_* to support-or-log; texture pipeline
  vs all glTF image sources + colorspace correctness; input vs the full
  gamepad/keyboard/mouse surface real games need).
- **Invariant enforcement (added 2026-08-18, claim-validation rulings):**
  the hardened tickets must carry, as explicit acceptance criteria where
  they apply: the D24 eviction invariant (residency-tolerant handle
  resolve on Tasks 13 and 19), D25 UploadTicket consumption at every new
  upload call site, the D26 GPU-driven-readiness invariants (per-draw
  addressing on Tasks 16/19, SoA indirect-compatible ViewLists +
  instancing collapse + caller-owned storage on Task 19, BDA enablement
  on Task 12), and the D27 main-thread pre-resolution answer to the
  `bindInstance`-on-workers collision — a correctness blocker Task 19
  must resolve BEFORE dispatch, not discover during it.

**Exit:** every Phase 4 ticket carries exhaustive acceptance criteria
grounded in a cited completeness matrix; the matrices are committed to
the SDD workspace; the feature-gap register absorbs any new findings.
This gate is COMPLETE before Task 10 dispatches.

### Task 10: Memory budget, accounting & eviction-invariant foundation (D24, card #27)

**Files:** Modify `src/rx_rhi_vk/src/buffer.cpp` (allocator creation:
`VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` when the extension is
present), `src/rx_rhi_vk/src/device.cpp` (opportunistic
`VK_EXT_memory_budget` enablement, logged either way), new
`src/rx_rhi_vk/include/rx_rhi_vk/memory_report.h` + impl (category-
attributed accounting + `vmaGetHeapBudgets` polling → POD
`RxMemoryReport`: per-category bytes/counts, heap usage/budget); audit
of every allocation site for `VK_ERROR_OUT_OF_DEVICE_MEMORY` handling
(loud named error + report attached — today there is zero handling
anywhere); tests.
**Scope split (D24):** this task lands accounting + budget query + host
report + OOM handling + the eviction CONTRACT (documented residency
semantics future subsystems implement). The residency-tolerant handle
resolve itself is implemented where handles live: Task 13 (registry)
and Task 19 (draw lists) carry it as acceptance criteria; this task
provides the contract text + report plumbing they cite. Eviction POLICY
(automatic what/when) stays streaming-phase (registry).
**Steps:** device-free tests (category accounting balance across
create/destroy; report POD layout), GPU test (budget query returns
sane nonzero values; forced small-budget path exercises the OOM error
path with a mock/`--budget-override`) → implement → both presets →
commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue27-memory-budget.md` as amended by
`gate/rulings-2026-08-18.md` §#27. Key deltas: `vmaSetCurrentFrameIndex`
wired once per frame (without it `vmaGetHeapBudgets` silently serves
stale values for up to 30 alloc/free ops — the matrix's load-bearing
omission), with a staleness regression test; the 5 enumerated OOM call
sites are the audit checklist (image-create vs image-view-create
failure classes become distinguishable); `RxMemoryReport` carries
per-HEAP budget/usage (RADV APUs expose multiple heaps by default —
never a heap-count assumption) + an explicit budget-source flag;
eviction contract text + minimal synthetic evict→fallback→reclaim
wiring test owned here, real call sites in Tasks 13/19.

### Task 11: Uploader completion tickets (D25, card #28)

**Files:** Modify `src/rx_rhi_vk/include/rx_rhi_vk/upload.h` +
`src/rx_rhi_vk/src/upload.cpp` (`UploadTicket flush()` — fence + ring
generation; `bool isComplete(UploadTicket)`; `void wait(UploadTicket)`;
ring reclamation keyed to ticket completion, not to having blocked;
`reserveRingSpace` wrap path waits only for the oldest in-flight ticket
covering the needed range), `mesh_buffers.h/.cpp`
(`MeshBuffers::create` keeps blocking semantics explicitly via
`wait(ticket)`, documented as convenience), sample 04's flush call
sites updated to poll, `src/rx_rhi_vk/src/device.cpp` (acquire optional
dedicated transfer queue via vk-bootstrap when present; graphics
fallback with logged degrade; exposed as
`Device::transferQueue()`/`hasDedicatedTransferQueue()` — acquisition
only, NO cross-queue submission this phase per D25/registry), tests.
**Constraints:** API stays main-thread-only (D5 unchanged); existing
callers that immediately `wait()` are byte-identical in behavior;
zero validation errors with sync validation.
**Steps:** tests: ticket completes exactly once; polling upload
overlapped with N rendered frames asserts main thread never blocks >
wall-clock threshold inside `flush()` (timer around the call, not frame
counters); ring-wrap under in-flight tickets reclaims correctly
(stress: many small uploads > ring size); `MeshBuffers::create`
unchanged behavior → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue28-upload-tickets.md` as amended by
`gate/rulings-2026-08-18.md` §#28 + RC4. Key deltas: the ticket
primitive is a TIMELINE SEMAPHORE (D25 amended — the current
single-reused-fence design cannot support pollable tickets;
reset-while-referenced hazard); the overlapping-flush discrimination
test (two flushes, first ticket still reports its own true status) is
the highest-priority gate; direct-path-only batches (UMA/ReBAR)
return already-complete tickets; the wall-clock test must FAIL against
pre-D25 code to prove it discriminates; transfer-queue acquisition
uses `get_dedicated_queue` (never `require_dedicated_transfer_queue`);
sample 04's three cited call sites are audited without claiming the
list exhaustive.

### Task 12: GeometryPool (D8/D9, D26.4)

**Files:** Create `src/rx_asset/{CMakeLists.txt,include/rx_asset/geometry_pool.h,geometry_pool.cpp,tests/...}`.
**Interfaces (produces):**
```cpp
namespace rx::asset {
struct MeshRange { uint32_t blockId; uint32_t firstIndex; uint32_t indexCount; int32_t vertexOffset; };
struct PoolVertex { float px,py,pz; float nx,ny,nz; float tx,ty,tz,tw; float u,v; }; // 48B, static_assert-pinned (D8)
class GeometryPool { // main-thread affinity (D5)
 public:
  static std::unique_ptr<GeometryPool> create(rhi::Device&, rhi::Uploader&, const PoolConfig& cfg /*chunk sizes, defaults 64MB/32MB*/);
  MeshRange upload(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices); // suballoc via VmaVirtualBlock; new chunk on exhaustion
  void free(const MeshRange&);                       // virtual-free; no defrag (D9)
  void bind(VkCommandBuffer cmd, uint32_t blockId) const; // vertex+index bind for a block
  PoolStats stats() const;                            // bytes used/capacity per block — Tracy plots fed by caller
};}
```
**Added acceptance criteria (2026-08-18, D25/D26.4):**
- `upload()` consumes the Task 11 `UploadTicket` path (no assumption of
  synchronous flush); pool block creation carries
  `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` when BDA is enabled.
- BDA enablement (D26.4): `bufferDeviceAddress` feature bit set
  opportunistically in `features12` (NEVER a device-selection
  requirement; logged degrade; lavapipe support verified in-task before
  any CI dependency), `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`
  set when on, `Device::supportsBufferDeviceAddress()` exposed; test
  asserts a pool block yields nonzero `vkGetBufferDeviceAddress` on a
  supporting device. Nothing consumes addresses in Phase 4 — enablement
  only, because it is unpurchasable later without reallocating and
  re-uploading the entire pool.
**Steps:** device-free tests impossible (GPU) — GPU tests: upload two meshes → distinct non-overlapping ranges; free+re-upload reuses space (stats assert); exhaustion → new block, both drawable (record real indexed draws from two blocks, readback probe); BDA assertions above; zero validation errors → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue21-geometry-pool.md` as amended by
`gate/rulings-2026-08-18.md` §#21. Key deltas: per-allocation
`VmaVirtualAllocationCreateInfo::alignment` = 48 (vertex) / 4 (index)
with structural divisibility asserts (VMA does not know element
strides — offsets must convert cleanly to vertexOffset/firstIndex);
`stats()` includes block COUNT; the checkerboard fragmentation test
proves no-defrag is observable behavior; "geometry-pool" category
label threaded into the D24 accounting; u32 index width stands
(16-bit sub-pools registered for the geometry phase).

### Task 13: Import core — fastgltf + MikkTSpace + meshoptimizer (D7)

**Files:** Vendor fastgltf + MikkTSpace + meshoptimizer (pinned, licenses recorded); create `src/rx_asset/{import_gltf.{h,cpp}},registry.{h,cpp},fallbacks.cpp`, `assets/test/cube_textured.gltf(+bin, committed, <20KB, hand-authored)`, `tools/fetch_assets.sh` (DamagedHelmet mandatory + `--sponza` optional; checksums; CI caches like slang-prebuilt); tests.
**Interfaces (produces):**
```cpp
namespace rx::asset {
using MeshHandle = core::Handle<struct MeshTag>; using TextureHandle = core::Handle<struct TextureTag>; using MaterialHandle = core::Handle<struct MatTag>;
struct Submesh { MeshRange range; AABB bounds; MaterialHandle material; };
struct MeshAsset { std::vector<Submesh> submeshes; AABB bounds; SkinData skin; /* preserved, unused (seed 14) */ };
struct ImportedScene { std::vector<InstanceRecord> instances; /* flattened world transforms (D12) + mesh handles */ };
class Registry { // owns all assets; main-thread mutation (D5)
  ImportResult importGltf(const std::filesystem::path&, GeometryPool&, /*Stage-1 Task 14 adds*/ TextureCache*);
  const MeshAsset& mesh(MeshHandle) const; /* + material/texture accessors, fallback handles (D11) */
};}
```
Pipeline per primitive: fastgltf parse → mandatory attributes (missing normals → flat-generate + warn) → tangents from file else MikkTSpace → meshopt sequence (remap→cache→overdraw→fetch [R:assets]) → AABB → pool upload. Node tree flattened to world-space InstanceRecords (D12). COLOR_0/TEXCOORD_1 logged-and-skipped. Materials parsed to parameter sets (textures resolved in Task 14; until then fallback handles D11).
**Added acceptance criteria (2026-08-18, per 2026-08-12 ledger rulings + D24/D25):**
- IO-source abstraction invariant: loaders read through a
  host-injectable byte source (fastgltf callback loading), NOT hardcoded
  `std::filesystem` — asset packaging/VFS is host policy (the
  path-taking convenience overload wraps the byte-source path).
- Eviction invariant (D24): Registry handle resolve is
  residency-tolerant (may yield fallback while nonresident);
  handle-mediated references only — no raw pointer/index escapes that
  break under eviction; one deferred-eviction path exercised in tests.
- Upload paths consume Task 11 tickets.
**Steps:** unit tests on committed cube (counts, AABB, tangent presence, meshopt actually ran — index order differs from source), DamagedHelmet integration test (fetched; counts/submeshes/skin-preservation assertions), error paths (missing file → fallback + log; garbage file → error result, no crash), byte-source injection test (import from an in-memory source, no filesystem), eviction-invariant test → implement → both presets → commit(s).
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue02-gltf-import.md` (full core + 60-extension
disposition table) as amended by `gate/rulings-2026-08-18.md` §#2.
Key deltas/re-rulings: vendoring budget grows to fastgltf v0.9.0
(+ embedded simdjson license) + meshoptimizer v1.2 + MikkTSpace
(pinned commit, header-license text quoted) + Draco
(`DRACO_GLTF_BITSTREAM=ON`); byte-source implemented via the
matrix-specified pattern (never `Options::LoadExternal*`; renderer
resolves every `sources::URI` itself — fastgltf has NO external-URI
callback); image/buffer source variants (URI/data-URI/GLB) are
CONSUME-NOW all three; KHR_texture_transform offset/scale CONSUME-NOW
(rotation logged) — gltfpack emits it by default with quantized UVs;
KHR_materials_unlit maps to the D22 Unlit material;
EXT_mesh_gpu_instancing consumed at import (TRS arrays expand into
InstanceRecords); u8/u16 index accessors widened to u32 with a value
round-trip test; overdraw threshold 1.05; the generic
extensionsUsed/Required surfacing mechanism (INFO summary +
UnknownRequiredExtension → named error + fallbacks) is what makes
log-don't-drop true for the whole registry.

### Task 14: KTX2 textures + sampler cache (D10)

**Files:** Vendor libktx (pinned; Apache-2.0 recorded); create `src/rx_asset/texture_cache.{h,cpp}`; extend importer material resolution; tests (+ tiny committed .ktx2 fixtures generated by documented `toktx` commands).
**Interfaces:** `TextureCache::load(path, TextureRole role)` → TextureHandle (bindless idx inside); role → transcode target + colorspace per D10 table; sampler cache keyed by (wrap,filter,aniso) → VkSampler, glTF samplers honored, aniso 8× default when supported; stb path for PNG/JPG with warning; checkerboard fallback on failure (D11); mips from container (warn if absent).
**Added (2026-08-18):** reads through the Task 13 byte-source abstraction (no direct filesystem in the load path); uploads consume Task 11 tickets; texture memory attributed in the Task 10 accounting categories.
**Steps:** tests: role→format matrix (BC7_SRGB/BC5/BC7_UNORM assertions on lavapipe-supported... **note**: lavapipe BC support — verify in-task; if a target format is unsupported on CI's driver, transcode falls back to RGBA8 with warning and the test asserts the fallback path on that driver, exact-format path asserted locally) — sampler dedup (two identical glTF samplers → one VkSampler), quadrant pixel GPU test sampling a loaded KTX2 → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue03-ktx2-textures.md` as amended by
`gate/rulings-2026-08-18.md` §#3. Key deltas: pin libktx **v4.4.2**
(stable; v5 is RC — registry watch item), vendoring commit records
Apache-2.0 PLUS bundled-component licenses actually compiled; KTX2
parse through `ktxStream`/`CreateFromMemory` (zero filesystem in
texture_cache.cpp — grep-enforced); glTF ROLE is authoritative for
the created format, container-DFD disagreement → one WARN (the Godot
#99589 double-sRGB class made loud); non-Basis KTX2 detected before
transcode (never discovered via KTX_INVALID_OPERATION); block-
compressed upload support in `Uploader::uploadToImage` (block-row
pitch, sub-block mip tails) is an explicit acceptance item; stb path
uploads mip 0 only — recorded limitation + WARN; role-appropriate
UTILITY fallbacks (flat-normal for normal slots, never checkerboard);
per-failure single-shot logging (no per-frame spam).

### Task 15: Async import pipeline (D5 contract in action)

**Files:** Modify `src/rx_asset/registry.{h,cpp}` (+`importGltfAsync(path, ..., CompletionFn)` — parse/decode/transcode/meshopt on workers via rx_task; the SYNC importGltf also parallelizes per-primitive work internally (parallelism is the default, not an async-only property), GPU uploads + registry mutation marshalled through postToMain; progress/Tracy zones), tests.
**Steps:** test: async import of cube + DamagedHelmet completes with identical results to sync path (deep compare of counts/ranges); main-thread-affinity assertions (registry mutation thread id checks in debug); a deliberately slow decode overlapped with rendered frames (frame loop keeps presenting — test drives N frames while import in flight, asserts no stall > threshold frames on counters **AND, added 2026-08-18 per D25, a wall-clock main-thread-block assertion: no single `pumpMain()`/upload call blocks the main thread beyond threshold — the counter-only criterion cannot detect a per-frame fence stall**); GPU-side handoff consumes Task 11 tickets (poll, never blocking wait, in the frame loop) → implement → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue22-async-import.md` as amended by
`gate/rulings-2026-08-18.md` §#22 + RC6. Key deltas: SCOPE GROWS with
abandon-style cancellation (`cancel(handle)`: atomic flag,
stage-boundary checks, no registry mutation after observation, defined
callback outcome, bounded latency, ASan-clean; prioritized cancel
stays streaming-phase) and a minimal poll-able progress snapshot
(stage enum + items-completed/total, monotonic — internal C++ this
phase); determinism via FILE-ORDER application at the marshal point
(results independent of worker count, run-to-run identical);
worker-stage bodies are exception-bounded (an exception across a chunk
boundary is process-fatal by design); wall-clock two-tier gate per the
amended D18 (2 ms local budget published; 10 ms CI stall detector);
teardown-with-import-in-flight is defined, leak-clean behavior;
concurrent imports complete isolated (no priorities, documented).

### Task 16: StandardPBR + Unlit + sample 08_gltf_viewer (D22)

**Files:** Create `shaders/material/standard_pbr.slang`, `shaders/material/unlit.slang` (public IMaterialShader modules — zero special treatment), extend material system only via existing public/spec'd seams (specialization bits gain alphaMode/doubleSided axes; BLEND pipeline-state variant per D22 wired through PassSignature/pipeline build); create `samples/08_gltf_viewer/` (DamagedHelmet default `--scene path` override; imports ASYNCHRONOUSLY by default via Task 15's pipeline with a rendered loading state — the viewer is the async demonstration vehicle; orbit camera (drag), manual `--exposure`; forward+tonemap; reversed-Z NOT yet — camera helpers arrive Stage 2, viewer uses existing conventions with a code comment referencing D13's Stage-2 migration); tolerance pixel gate vs committed 256² lavapipe references (D17, regeneration script `tools/regen_references.sh`); packaging/CI.
**Added acceptance criteria (2026-08-18, D26.1):** both material
shaders receive per-draw data via `firstInstance`/`gl_InstanceIndex`
indexing into a bindless storage buffer — never per-draw push constants
— so the scene path (Task 19's `recordDrawList`) can drive them
unmodified and a future indirect path can too. Sample 07's
push-constant-per-draw loop is the recorded anti-pattern; the material
interface must not inherit it.
**Steps:** material unit tests (params reflect; alpha variants produce distinct pipelines; MASK cutoff pixel test; BLEND draws blended — quadrant test) → viewer + gate → packaging → numbers in report (import ms via Tracy, first-frame ms) → commit(s).
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue08-standard-pbr.md` as amended by
`gate/rulings-2026-08-18.md` §#8 + RC1. Key deltas: FILE LIST GROWS —
`shaders/material/forward_entry.slang` + `material.slang` (tangent
field + lighting surface are unlisted prerequisites for normal
mapping); D28 lands here (MaterialRecord fixed-function state axis —
the "specialization bits gain alphaMode/doubleSided" phrasing above
is CORRECTED: those are pipeline fixed-function fields, not spec
constants; MASK cutoff = per-instance uniform + always-present
discard); D26.1 shaders use **`SV_VulkanInstanceID`** (Slang subtracts
firstInstance from `SV_InstanceID` — verified pitfall; code comment +
a two-draw firstInstance>0 test are mandatory); per-draw data read
from a bindless StructuredBuffer matching Task 19's DrawPayload
(legacy bindInstance/set-1 path retained for non-scene samples only,
scoped explicitly); KHR_texture_transform offset/scale applied
in-shader (per the #2 re-ruling); Lambertian diffuse +
GGX/Smith-correlated/Schlick baseline (three first-tier references
converge); FG1 ambient closed-form metal-probe test; `--exposure` =
pre-tonemap 2^exposure push constant with a neutral-value regression
guard (shared tonemap shaders untouched); BC5 Z-reconstruction with
radicand clamp; sample 08's drag-orbit reads SDL mouse state directly
via `Window::sdlWindow()` (sample-local; rx_platform's real input
surface stays Task 20 — sequencing gap resolved without reordering).

### Task 17: Window edge-state hardening (FG7, card #25)

**Files:** Modify `src/rx_rhi_vk/device.cpp` (`recreateSwapchain`
zero-extent guard — 0×0 extent skips recreation and enters a
suspended-present state, resumed on restore), `src/rx_platform/window.cpp`
(+header) (SDL3 minimize/restore/occluded events surfaced;
windowed/borderless-fullscreen toggle via the plain SDL3 window flag on
the existing recreation machinery), samples gain `--fullscreen` where
present-mode flags already exist; tests + MANUAL_VERIFICATION rows
(minimize/restore under present, alt-tab).
**Rationale:** FG7 (feature-gap audit): a 0×0 swapchain recreate fails
validation or crashes; minimize is the first thing a playtester does.
Occlusion + DPI policy stay SDK-phase (registry).
**Steps:** device-free guard tests where possible; GPU test drives
resize-to-zero → restore sequence headlessly (event injection), asserts
no validation errors and rendering resumes; manual rows for true
minimize under a live swapchain → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue25-window-hardening.md` as amended by
`gate/rulings-2026-08-18.md` §#25. DESIGN CORRECTION: the
suspended-present guard is **extent-query-driven, not event-driven** —
on Wayland (SteamOS desktop mode included) SDL minimize events/flags
do not fire on compositor-driven minimize and `currentExtent` is a
sentinel, so events are an optimization/log layer, never the sole
gate. Key deltas: guard gets a dependency-injection seam (extent as a
parameter) so its logic is testable without a display that reports
zero; two-tier tests (SDL_PushEvent state-machine tests + seam-
injected GPU guard test; true OS minimize is MANUAL-only, honestly
recorded); borderless = `SDL_SetWindowFullscreen(w,true)` + NULL mode
+ `SDL_SyncWindow()` before readback, double-toggle test; present
skip asserted by CALL COUNTS (0 acquire/present over N suspended
frames); one-shot Wayland-limitation log line; uploads NOT gated on
presentation while suspended (D25 polling continues); hidden-window
CI extent question resolved empirically at implementation.

---

## STAGE 2 — Scene & Culling

### Task 18: Scene proxies (`rx_scene`, D19) + reversed-Z camera (D13)

**Files:** Create `src/rx_scene/{CMakeLists.txt,include/rx_scene/{scene.h,camera.h},scene.cpp,tests/...}`.
**Interfaces (produces):**
```cpp
namespace rx::scene {
struct Camera { /* pos/orientation, vfov, near; reversed-inf-far projection helpers (D13): proj(), viewProj(); cullMask u32 = ~0u */ };
using RenderableHandle = ...; using LightHandle = ...;
struct RenderableDesc { asset::MeshHandle mesh; /* per-submesh material overrides optional */ glm::mat4 transform; uint32_t layers = ~0u; uint8_t channels = 0xFF; };
struct DirectionalLightDesc { glm::vec3 dir; glm::vec3 colorLux; bool castsShadows; uint8_t channels = 0xFF; };
class Scene { // SoA managers inside (transform pool carries prev-frame slot layout per seed-8/temporal note)
  RenderableHandle createRenderable(const RenderableDesc&);
  void setTransform(RenderableHandle, const glm::mat4&); void setLayers(RenderableHandle, uint32_t); /* destroy, light equivalents */
};}
```
Reversed-Z: depth attachment usage in samples migrating in Task 22/24; Camera helpers are the single source of projection truth; unit tests assert near→1/far→0 mapping and frustum plane extraction correctness.
**Steps:** device-free tests (handle lifecycle incl. generational failure, SoA iteration order, prev-transform slot updated on setTransform) → implement → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue05-scene-proxies.md` as amended by
`gate/rulings-2026-08-18.md` §#5 + RC5. Key deltas: reuse
`rx::core::Handle<Tag>` (the TYPE — not the AoS `HandlePool`; storage
is SoA columns per D19, span-accessor test proves it);
`RenderableDesc` gains `castsShadows: bool = true` (RC5) and
`priority: uint8_t` (0-7, default 4 — reserved sort tier, Filament
precedent); per-submesh material override becomes a concrete typed
field (span of optional MaterialHandle), not a comment; reserved
skinning/morph slots (inert, tested); `Camera` gains `cullingProj()`
(separate FINITE culling projection — Gribb-Hartmann degenerates
under D13's infinite far; Filament's own answer) and an inert jitter
offset; exposure STAYS on the tonemap (D22 stands — camera exposure
API registered for the techniques phase with physical units);
no-dirty-tracking is itself a criterion (setTransform = O(1) SoA
write; 30k-call benchmark published); D24 residency-tolerant resolve
test at the proxy level.

### Task 19: DrawListBuilder — parallel culling + sort keys (D14/D15, D26, D27)

**Files:** Create `src/rx_scene/draw_list.{h,cpp}` + tests.
**Interfaces (amended 2026-08-18 per D26 — SoA, indirect-compatible, caller-owned storage):**
```cpp
// Geometry fields packed VkDrawIndexedIndirectCommand-compatible; per-draw payload in a
// parallel array (SoA). Grouped by GeometryPool blockId — the recorded per-block indirect
// submission bound (one future MDI call per block).
struct DrawCommand { uint32_t indexCount, instanceCount, firstIndex; int32_t vertexOffset; uint32_t firstInstance; };
struct DrawPayload { uint32_t materialIndex; uint32_t instanceDataIndex; };  // resolved from asset::MaterialHandle (T13 Registry, residency-tolerant per D24)
struct ViewLists { std::vector<DrawCommand> commands; std::vector<DrawPayload> payloads; std::vector<BlockRange> blocks; CullCounters counters; /* opaque FtB by u64 key pipeline|material|depth, then instancing-collapsed; blend BtF, uncollapsed */ };
class DrawListBuilder { // caller-owned reused storage: zero net allocations in steady state
  void build(const Scene&, const Camera&, task::Scheduler&, ViewLists& out);
  void buildShadow(const Scene&, const DirectionalLight&, const Camera&, task::Scheduler&, ShadowLists& out);
};
// Engine-provided chunked submit helper (parallel recording is the DEFAULT for scene-driven passes):
// rx::scene::recordDrawList(PassContext&, chunkIndex, chunkCount, const ViewLists&, ...) — sample 09 hand-chunks nothing.
```
Frustum cull: planes from reversed-Z viewProj; AABB-vs-planes batched in parallelFor chunks (grain ~512); layer masks (camera cullMask, light channels incl. caster filtering); shadow ortho frustum fitted to camera-visible bounds + conservative caster extrusion along light dir (D15). Sort per D14. Counters exact (CI-gateable).
**Amendments (2026-08-18):**
- **Instancing collapse (D26.3, seed 9c):** after sorting, adjacent
  identical (pipeline, material, mesh range, block) runs collapse into
  one `DrawCommand` with `instanceCount > 1`; counters report
  records-in vs draws-submitted (CI-gated in sample 09's stress-v2).
- **Per-draw addressing (D26.1):** `recordDrawList` drives materials via
  `firstInstance` indexing into the bindless per-draw buffer — zero
  per-draw push constants in the scene path.
- **Main-thread pre-resolution (D27):** before fan-out, the helper
  pre-resolves every distinct (material, pass-signature, specialization)
  pipeline + parameter offsets on the main thread from the sorted list;
  worker chunks consume only pre-resolved plain data — `getPipeline`/
  `bindInstance` are main-thread-guarded and MUST NOT be called from
  chunks ≥ 1 (the sample-06 collision, resolved here by design).
- **Eviction invariant (D24):** material/mesh handle resolution at
  list-build time is residency-tolerant (fallback substitution, never a
  crash or raw-pointer escape).
- **Zero-alloc invariant:** `build()` into reused storage performs zero
  net heap allocations across steady-state frames — asserted by test.
**Steps:** device-free tests with synthetic scenes: known in/out AABB sets (exact counters), mask filtering matrices, sort-order assertions (opaque key monotonic, blend depth descending), instancing-collapse assertions (identical-run scene → 1 command with instanceCount=N; counters match), off-screen-caster-still-casts case, determinism across thread counts (same lists any --threads), steady-state zero-allocation assertion, pre-resolution unit test (worker chunks never hit the main-thread guard — assert under a debug hook) → implement → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue06-drawlists-culling.md` +
`gate/matrix-issue07-layer-masks.md` as amended by
`gate/rulings-2026-08-18.md` §#6/#7 + RC3/RC5. Key deltas: sort-key
bit layout documented with named constants + decode() round-trip test
(bgfx discipline); depth bucket = truncated monotonic float32 bit
pattern (never linear rescale); deterministic low-bit tie-break =
stable creation index; partition sort-DIRECTION test on one shared
fixture (opaque decreasing / blend increasing under reversed-Z);
priority tier above pipeline bits, blendOrder bits reserved
unpopulated; fixed index-range chunks + chunk-index-ordered
concatenation → byte-identical output across --threads; **D26.3
lockstep criterion**: commands and payloads sort/collapse as ONE unit
— the interleaved-scene test that catches silent payload
desynchronization is mandatory (collapsed `[firstInstance,
firstInstance+instanceCount)` ranges cross-checked against source
renderables); `CullCounters` = {totalCandidates, culledByLayerMask,
culledByFrustum, visible, recordsIn, drawsSubmitted,
shadowCastersConsidered, shadowCastersVisible} — exact, CI-gated;
`ShadowLists` = ViewLists shape, single partition, sorted (pipeline,
mesh range, block), BLEND excluded (RC3); culling planes from
`Camera::cullingProj()` (finite) + the extreme-depth never-culled
test; degenerate/zero-extent AABB + ground-slab cases; per-block
contiguous `BlockRange` grouping test; D27 worker-guard test reuses
`setViolationHookForTests` + the rendezvous-barrier pattern from
test_material_system.cpp:854-1042 verbatim; layer/channel getters +
`setChannels` added; the five-case mask CI matrix.

### Task 20: Input expansion (seed 6) — Haiku

**Files:** Modify `src/rx_platform/{include/rx_platform/window.h,window.cpp}` (+input.h if cleaner per existing layout): relative mouse mode (SDL_SetWindowRelativeMouseMode), per-frame accumulated mouse deltas from SDL_EVENT_MOUSE_MOTION xrel/yrel, cursor show/hide, gamepad: hot-plug via SDL_EVENT_GAMEPAD_ADDED/REMOVED, `GamepadState poll()` (left/right stick float2 with 8000/32768 deadzone [R:present], triggers, A/B buttons); tests where device-free (deadzone math), manual rows for the rest.
**Steps:** per existing rx_platform test conventions → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue14-input.md` as amended by
`gate/rulings-2026-08-18.md` §#14. Key deltas: SDL3 has NO
`SDL_GAMEPAD_BUTTON_A/_B` — the surface exposes the full discrete set
(D-pad ×4, face ×4 SOUTH/EAST/WEST/NORTH, both shoulders, START) as a
bitmask/bool struct; deadzone is **scaled-radial per stick** (mag =
|stick|/32768; below 8000/32768 → zero; else normalize × rescale —
never two per-axis clamps; the (8500,8500) discrimination case is
mandatory) + a 1D trigger deadzone (~1000/32767, tunable); mouse
deltas = xrel/yrel event accumulation with consume-and-reset;
focus-loss pause + unconditional re-arm of relative mode on
focus-gain; cursor confine via `SDL_SetWindowMouseGrab` in scope;
hot-plug map keyed by SDL_JoystickID (close-then-erase synchronously);
single-active rule = lowest connected ID, virtual-joystick-tested;
TEST-COVERAGE CORRECTION: `SDL_AttachVirtualJoystick` makes hot-plug/
axis/button paths CI-automatable (the "manual rows for the rest"
phrasing above is superseded; a smoke run confirms virtual joysticks
work under CI's driver first); **SCOPE GROWS: minimal keyboard
surface** — `bool isKeyDown(SDL_Scancode)` over `SDL_GetKeyboardState`
(sample 09's WASD fly-through requires it; its absence was an
oversight); rumble/touchpad deferred (registry, SDK/platform); gyro =
log-don't-drop with HasSensor + device-name logging (SDL #9148: Deck
gyro/paddles undetectable at our pin); thread-affinity one-liners +
RX_ASSERT_MAIN_THREAD guards per the in-repo convention; input
accumulators gate on ImGui's WantCapture flags (Task 21 coordination
— single event-dispatch owner in pumpEvents()).

### Task 21: `rx_debug_ui` — ImGui overlay module (D20)

**Files:** Vendor imgui v1.92.x (pinned, MIT recorded; core + sdl3 + vulkan backends only); create `src/rx_debug_ui/{CMakeLists.txt,include/rx_debug_ui/overlay.h,overlay.cpp}`.
**Interfaces:** `Overlay::create(Device&, Window&, format)` (own descriptor pool sized per [R:present]; font upload via existing Uploader; UseDynamicRendering with swapchain format); `beginFrame()` (SDL event feed already flowing through Window — overlay hooks the existing event dispatch), `addPass(RenderGraph&, targetName)` — declares a graph pass (side-effect, reads nothing) whose callback renders draw data; core libs stay ImGui-free (only samples + rx_debug_ui link it).
**Steps:** GPU smoke test (overlay pass renders; readback shows non-empty overlay region with a forced demo window; zero validation errors) → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue16-imgui-overlay.md` as amended by
`gate/rulings-2026-08-18.md` §#16. Key deltas: pin **v1.92.9b**
(plain tag, NOT -docking — structural exclusion); init fills the
NESTED `PipelineInfoMain.PipelineRenderingCreateInfo` (2025-09-26
breaking change — the flat field no longer exists); descriptor pool =
two typed entries (SAMPLED_IMAGE ≥ 8 + SAMPLER ≥ 2,
FREE_DESCRIPTOR_SET_BIT; 2026-04-22 breaking change); FONT-UPLOAD
RULING: the backend's lazy internal upload (raw vkAllocateMemory +
`vkQueueWaitIdle` inside RenderDrawData — vendored, unmodifiable) is
a DOCUMENTED exception to D25/D24: force texture creation at init so
the stall is one-time, assert at-most-once QueueWaitIdle across an
N-frame run (guards against per-frame texture churn), note the D24
accounting blind spot in the memory report's docs;
`ImGui_ImplSDL3_SetGamepadMode(Manual, nullptr, 0)` immediately after
init (kills the AutoFirst race with Task 20's gamepad ownership;
gamepad HUD nav off this phase); every SDL event →
`ImGui_ImplSDL3_ProcessEvent` FIRST, then platform input; pass
declaration uses the REAL API chain — `addPass().addColorOutput(
target, LOAD_OP_LOAD).setSideEffect().setExecute(...)` — LOAD-not-
CLEAR is a named criterion with the pattern-visible-under-HUD test;
configure-time CMake link-boundary check (imgui absent from every
rx_* core target's transitive closure) lands here; main-thread
one-liner per convention.

### Task 22: Shadow quality bridge (D21)

**Files:** Modify `shaders/multipass/` shadow path shared pieces as needed → but primary target is the Stage-2 scene shadow path: light ortho fitted to visible bounds (from DrawListBuilder), slope-scaled depth bias (vkCmdSetDepthBias on the shadow pass), 3×3 PCF in the standard lit path (`shaders/material/forward_entry.slang` shadow helper upgrade; sample 05 keeps its own simpler shaders untouched — documented). Reversed-Z main-camera migration lands here for the scene path (clear values, compare ops via PassSignature/pipeline state).
**Steps:** GPU test: acne scene (large ground plane at grazing light) renders without acne (probe variance check) and without peter-panning (contact probe); PCF softness probe (edge gradient spans ≥2 texels) → implement → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue23-shadow-bridge.md` as amended by
`gate/rulings-2026-08-18.md` §#23 + RC2/RC3. Key deltas: FILE LIST
GROWS — `src/rx_graph/{resources.h,executor.cpp}` (D29: AttachmentDesc
`DepthConvention` + per-pass clear values at both executor sites; the
two-convention one-frame test); the shadow pass STAYS standard-Z per
D13, so the depth-bias sign does NOT flip — a required code comment
prevents the plausible-but-wrong "reversed-Z fix" (main-camera
migration lands in the same task); shadow-caster pipeline is built
OUTSIDE MaterialSystem (RC3 option (a) — getPipeline's compare-op
flips as one literal for the main camera; no compare-op axis);
`VK_DYNAMIC_STATE_DEPTH_BIAS` added to the shadow pipeline's dynamic
state (the creation-time detail the ticket omitted);
`depthClampEnable=VK_TRUE` on casters + device-feature check;
comparison-sampler PCF (compareEnable=TRUE, COMPARE_OP_LESS,
SampleCmp-equivalent taps — hardware filtering, not sample 05's manual
compare); texel snapping in scope (two-camera-position shimmer test);
shadow vertex shader uses SV_VulkanInstanceID bindless addressing
(never a push-constant transformIndex); 1024/D32_SFLOAT default kept
but parameterized; acne probe = neighborhood VARIANCE check at ≥80°
grazing, peter-panning probe = caster-base/shadow-edge continuity
within pixel tolerance.

### Task 23: Executor per-frame allocation elimination (card #29)

**Files:** Modify `src/rx_graph/executor.cpp` (+`Impl` in its header if
split); tests in the rx_graph targets.
**Scope (2026-08-18 claim-validation finding — 7 verified per-frame
heap-allocation sites in `execute()`; small, local, no public API
change):**
1. `execute()` performs **zero heap allocations in steady state**
   (unchanged graph, unchanged resource count) — asserted by a counting
   allocator hook or capacity-snapshot check across N frames.
2. The four per-execute tracking containers (`firstBarrierSeen`,
   `attachmentEverWritten`, `finalStageThisExecute`,
   `finalAccessThisExecute`) become `Impl`-persistent, cleared per
   frame; the two `unordered_map`s become index-addressed vectors
   (physical indices are dense).
3. Per-pass scratch (`colorPhysIdx`, `colorAttachments`, both barrier
   vectors, chunk command-buffer vectors) becomes reusable `Impl`
   scratch buffers.
4. Debug-label path stops constructing a `std::string` per pass
   (reusable null-terminated buffer); `nameToIndex` gains heterogeneous
   `string_view` lookup so per-pass resolver calls stop allocating.
**Constraints:** byte-identical rendering (existing GPU suite +
sample 05/06 pixel gates prove it); sample 07 numbers unchanged or
better; zero validation errors. `compile()`/`realize()` are exempt
(setup/resize-only paths, documented as such in the code).
**Steps:** allocation-count test first (fails on current code) →
implement → suite green both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue29-executor-allocations.md` as amended by
`gate/rulings-2026-08-18.md` §#29. Key deltas: methodology =
CAPACITY-SNAPSHOT via test-only accessors (per the in-repo
`debugChunkStats()` seam convention) — NO global operator-new
interposition (volk/validation-layer/Tracy-rpmalloc linkage risk);
test runs the representative Tracy-ON config, with Tracy's own
dynamic-zone-name allocations (verified: `tracy_malloc` per
RX_ZONE_DYNAMIC_NAME call at executor.cpp:1024/:1359) documented as
third-party-attributed exceptions (capacity snapshots are naturally
blind to them); the GPU-zone `VkCtxScope` allocation trace is
completed in-task; SCOPE GROWS: `DeletionQueue::onFrameFenceSignaled`'s
non-empty-path fresh-vector allocation (deletion_queue.cpp:40-41) is
folded in (in-place compaction; the pending-item test variant is
mandatory); `acquireChunkCommandBuffer` (executor.cpp:704-745) is the
named in-repo amortization template; `nameToIndex` gains a transparent
hash functor (documented libstdc++/libc++/MSVC string/string_view
hash-equivalence assumption) with a hit+miss overload-selection test.

### Task 24: Sample 09_scene + stress-v2 + release prep

**Files:** Create `samples/09_scene/` — loads DamagedHelmet field (headless: instanced helmets grid; present: `--scene sponza` when fetched) through Registry→Scene→DrawListBuilder→graph; fly-through camera (WASD + mouse capture + gamepad via Task 20's input surface — gate correction 2026-08-18: the earlier "D16 input" citation was a mislabel, D16 is test content); ImGui HUD: FPS/frame-ms, cull counters, vsync toggle, layer-mask toggles (hide/show instance groups), light-channel demo toggle, pool stats; stress-v2 mode `--stress` (same 30k-draw workload as sample 07 but through the full scene path — publishes A/B numbers vs 07 in the report + release notes); headless gate: counter assertions + tolerance pixels; MANUAL_VERIFICATION rows; packaging/CI; README/roadmap updates; `docs/superpowers/specs` layer table tick for layer 8.
**Steps:** TDD gate → implement → numbers (desktop; Deck rows added to MANUAL_VERIFICATION as unchecked) → packaging → commit(s).
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue15-sample09.md` (incl. the full
subsystem-exercise checklist, rows 3-17) as amended by
`gate/rulings-2026-08-18.md` §#15. Key deltas: **A/B comparability
contract** — held identical vs sample 07: 30k instances, the 4
pipeline/material variations, per-instance data, --threads/--vsync
semantics; expected to differ: draws SUBMITTED (07 submits all 30k
unconditionally; 09 culls+collapses) — published numbers report
wall-clock AND draws-submitted JOINTLY (wall-clock alone would
misrepresent the comparison; sample 07's own report format stays
untouched); collapse-ratio formula BLESSED: `1 −
drawsSubmitted/recordsIn` as a percentage, CI-asserted against the
hand-computed grid expectation; HUD ships TWO visibly distinct mask
controls (camera cullMask u32 ≠ light channels u8 — never conflated);
vsync toggle drives the same setPresentMode+recreate path as the CLI
flag; 60-frame rolling FPS average; headless gate asserts EXACT
counters (imported/visible/culled/recordsIn/drawsSubmitted) + a D17
tolerance reference via Task 16's regen script (no second mechanism);
packaged samples ship PRE-STAGED assets (fetched at package time —
must run standalone); `package_samples.sh` list + stale header count
fixed to include 08 AND 09, and the missing 07_stress
MANUAL_VERIFICATION section is added while touching that file;
registry layer-8 row annotated "(delivered: Phase 4 — scene
submission/culling; LOD remains deferred)" — the qualified form;
Task 23 lands before any stress-v2 number is published.

---

## Execution notes (coordinator)

- Models: Stage-0 Task 8 and Task 20 (input) Haiku (mechanical, fully specified); all others Sonnet; reviews all Sonnet; final whole-phase review at phase end.
- Sequencing: Stage 0: T1→T2→T3 sequential (T3 zones touch files broadly); T4/T5/T6 parallelizable in worktrees after T3 (disjoint); T7 after T2+T6; T8 anytime after T4 (Haiku, disjoint files). Stage 1: T9(gate)→T10→T11→T12→T13→T14→T15→T16, with T17 (window hardening) parallelizable any time after the gate (disjoint platform/present files; worktree). T10 before T11 (both touch allocator/device creation). Stage 2: T18→T19→{T20,T21 parallel}→T22→T24; T23 (executor cleanup) parallelizable with T20–T22 (rx_graph-only, worktree) but MUST land before T24's stress-v2 numbers.
- **Task renumbering (2026-08-18):** the primary-gate insertion left two "Task 9" headings and stale references; tasks are now uniquely numbered. Mapping for older ledger/report references — old T9 (GeometryPool)→T12, old T10 (import core/registry)→T13, old T11 (KTX2)→T14, old T12 (async import)→T15, old T13 (StandardPBR/viewer)→T16, old T14 (scene proxies)→T18, old T15 (draw lists)→T19, old T16 (input)→T20, old T17 (ImGui)→T21, old T18 (shadows)→T22, old T19 (sample 09)→T24. New tasks: T10 (memory budget, card #27), T11 (upload tickets, card #28), T17 (window hardening, card #25), T23 (executor allocation elimination, card #29).
- Each stage ends with a coordinator checkpoint: suite green both presets, stage sample packaged and run standalone, numbers recorded in ledger, board cards moved, then next stage dispatches.
- Phase exit: final whole-phase review (fresh, most scrutiny on cross-stage seams: registry↔scene handle lifetimes, parallel recording under real scene loads, threading contract adherence), one fix wave, push, CI green, tag v0.4.0-phase4, release with both packages + published numbers, board cards closed.
