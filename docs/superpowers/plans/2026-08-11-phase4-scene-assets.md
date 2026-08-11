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
  meshoptimizer's decode is already vendored), KHR_mesh_quantization,
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

**Exit:** every Phase 4 ticket carries exhaustive acceptance criteria
grounded in a cited completeness matrix; the matrices are committed to
the SDD workspace; the feature-gap register absorbs any new findings.
This gate is COMPLETE before Task 10 (GeometryPool) dispatches. (Tasks
10+ below are the former Tasks 9+, renumbered by this insertion.)


### Task 9: GeometryPool (D8/D9)

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
**Steps:** device-free tests impossible (GPU) — GPU tests: upload two meshes → distinct non-overlapping ranges; free+re-upload reuses space (stats assert); exhaustion → new block, both drawable (record real indexed draws from two blocks, readback probe); zero validation errors → implement → both presets → commit.

### Task 10: Import core — fastgltf + MikkTSpace + meshoptimizer (D7)

**Files:** Vendor fastgltf + MikkTSpace + meshoptimizer (pinned, licenses recorded); create `src/rx_asset/{import_gltf.{h,cpp}},registry.{h,cpp},fallbacks.cpp`, `assets/test/cube_textured.gltf(+bin, committed, <20KB, hand-authored)`, `tools/fetch_assets.sh` (DamagedHelmet mandatory + `--sponza` optional; checksums; CI caches like slang-prebuilt); tests.
**Interfaces (produces):**
```cpp
namespace rx::asset {
using MeshHandle = core::Handle<struct MeshTag>; using TextureHandle = core::Handle<struct TextureTag>; using MaterialHandle = core::Handle<struct MatTag>;
struct Submesh { MeshRange range; AABB bounds; MaterialHandle material; };
struct MeshAsset { std::vector<Submesh> submeshes; AABB bounds; SkinData skin; /* preserved, unused (seed 14) */ };
struct ImportedScene { std::vector<InstanceRecord> instances; /* flattened world transforms (D12) + mesh handles */ };
class Registry { // owns all assets; main-thread mutation (D5)
  ImportResult importGltf(const std::filesystem::path&, GeometryPool&, /*Stage-1 Task 11 adds*/ TextureCache*);
  const MeshAsset& mesh(MeshHandle) const; /* + material/texture accessors, fallback handles (D11) */
};}
```
Pipeline per primitive: fastgltf parse → mandatory attributes (missing normals → flat-generate + warn) → tangents from file else MikkTSpace → meshopt sequence (remap→cache→overdraw→fetch [R:assets]) → AABB → pool upload. Node tree flattened to world-space InstanceRecords (D12). COLOR_0/TEXCOORD_1 logged-and-skipped. Materials parsed to parameter sets (textures resolved in Task 11; until then fallback handles D11).
**Steps:** unit tests on committed cube (counts, AABB, tangent presence, meshopt actually ran — index order differs from source), DamagedHelmet integration test (fetched; counts/submeshes/skin-preservation assertions), error paths (missing file → fallback + log; garbage file → error result, no crash) → implement → both presets → commit(s).

### Task 11: KTX2 textures + sampler cache (D10)

**Files:** Vendor libktx (pinned; Apache-2.0 recorded); create `src/rx_asset/texture_cache.{h,cpp}`; extend importer material resolution; tests (+ tiny committed .ktx2 fixtures generated by documented `toktx` commands).
**Interfaces:** `TextureCache::load(path, TextureRole role)` → TextureHandle (bindless idx inside); role → transcode target + colorspace per D10 table; sampler cache keyed by (wrap,filter,aniso) → VkSampler, glTF samplers honored, aniso 8× default when supported; stb path for PNG/JPG with warning; checkerboard fallback on failure (D11); mips from container (warn if absent).
**Steps:** tests: role→format matrix (BC7_SRGB/BC5/BC7_UNORM assertions on lavapipe-supported... **note**: lavapipe BC support — verify in-task; if a target format is unsupported on CI's driver, transcode falls back to RGBA8 with warning and the test asserts the fallback path on that driver, exact-format path asserted locally) — sampler dedup (two identical glTF samplers → one VkSampler), quadrant pixel GPU test sampling a loaded KTX2 → implement → both presets → commit.

### Task 12: Async import pipeline (D5 contract in action)

**Files:** Modify `src/rx_asset/registry.{h,cpp}` (+`importGltfAsync(path, ..., CompletionFn)` — parse/decode/transcode/meshopt on workers via rx_task; the SYNC importGltf also parallelizes per-primitive work internally (parallelism is the default, not an async-only property), GPU uploads + registry mutation marshalled through postToMain; progress/Tracy zones), tests.
**Steps:** test: async import of cube + DamagedHelmet completes with identical results to sync path (deep compare of counts/ranges); main-thread-affinity assertions (registry mutation thread id checks in debug); a deliberately slow decode overlapped with rendered frames (frame loop keeps presenting — test drives N frames while import in flight, asserts no stall > threshold frames on counters not wall-clock) → implement → commit.

### Task 13: StandardPBR + Unlit + sample 08_gltf_viewer (D22)

**Files:** Create `shaders/material/standard_pbr.slang`, `shaders/material/unlit.slang` (public IMaterialShader modules — zero special treatment), extend material system only via existing public/spec'd seams (specialization bits gain alphaMode/doubleSided axes; BLEND pipeline-state variant per D22 wired through PassSignature/pipeline build); create `samples/08_gltf_viewer/` (DamagedHelmet default `--scene path` override; imports ASYNCHRONOUSLY by default via Task 12's pipeline with a rendered loading state — the viewer is the async demonstration vehicle; orbit camera (drag), manual `--exposure`; forward+tonemap; reversed-Z NOT yet — camera helpers arrive Stage 2, viewer uses existing conventions with a code comment referencing D13's Stage-2 migration); tolerance pixel gate vs committed 256² lavapipe references (D17, regeneration script `tools/regen_references.sh`); packaging/CI.
**Steps:** material unit tests (params reflect; alpha variants produce distinct pipelines; MASK cutoff pixel test; BLEND draws blended — quadrant test) → viewer + gate → packaging → numbers in report (import ms via Tracy, first-frame ms) → commit(s).

---

## STAGE 2 — Scene & Culling

### Task 14: Scene proxies (`rx_scene`, D19) + reversed-Z camera (D13)

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
Reversed-Z: depth attachment usage in samples migrating in Task 18/19; Camera helpers are the single source of projection truth; unit tests assert near→1/far→0 mapping and frustum plane extraction correctness.
**Steps:** device-free tests (handle lifecycle incl. generational failure, SoA iteration order, prev-transform slot updated on setTransform) → implement → commit.

### Task 15: DrawListBuilder — parallel culling + sort keys (D14/D15)

**Files:** Create `src/rx_scene/draw_list.{h,cpp}` + tests.
**Interfaces:**
```cpp
struct DrawRecord { asset::MeshRange range; uint32_t blockId; asset::MaterialHandle mat; uint32_t instanceIndex; };  // asset-level material handle (T10 Registry resolves to the bindable material instance at record time)
struct ViewLists { std::vector<DrawRecord> opaque /*sorted FtB by u64 key: pipeline|material|depth*/, blend /*BtF*/; CullCounters counters; };
class DrawListBuilder { ViewLists build(const Scene&, const Camera&, task::Scheduler&); ShadowLists buildShadow(const Scene&, const DirectionalLight&, const Camera&, task::Scheduler&); };
// PLUS the engine-provided chunked submit helper (parallel recording becomes the DEFAULT for scene-driven passes):
// rx::scene::recordDrawList(PassContext&, chunkIndex, chunkCount, const ViewLists&, ...) — sample 09 hand-chunks nothing.
```
Frustum cull: planes from reversed-Z viewProj; AABB-vs-planes batched in parallelFor chunks (grain ~512); layer masks (camera cullMask, light channels incl. caster filtering); shadow ortho frustum fitted to camera-visible bounds + conservative caster extrusion along light dir (D15). Sort per D14. Counters exact (CI-gateable).
**Steps:** device-free tests with synthetic scenes: known in/out AABB sets (exact counters), mask filtering matrices, sort-order assertions (opaque key monotonic, blend depth descending), off-screen-caster-still-casts case, determinism across thread counts (same lists any --threads) → implement → commit.

### Task 16: Input expansion (seed 6) — Haiku

**Files:** Modify `src/rx_platform/{include/rx_platform/window.h,window.cpp}` (+input.h if cleaner per existing layout): relative mouse mode (SDL_SetWindowRelativeMouseMode), per-frame accumulated mouse deltas from SDL_EVENT_MOUSE_MOTION xrel/yrel, cursor show/hide, gamepad: hot-plug via SDL_EVENT_GAMEPAD_ADDED/REMOVED, `GamepadState poll()` (left/right stick float2 with 8000/32768 deadzone [R:present], triggers, A/B buttons); tests where device-free (deadzone math), manual rows for the rest.
**Steps:** per existing rx_platform test conventions → implement → both presets → commit.

### Task 17: `rx_debug_ui` — ImGui overlay module (D20)

**Files:** Vendor imgui v1.92.x (pinned, MIT recorded; core + sdl3 + vulkan backends only); create `src/rx_debug_ui/{CMakeLists.txt,include/rx_debug_ui/overlay.h,overlay.cpp}`.
**Interfaces:** `Overlay::create(Device&, Window&, format)` (own descriptor pool sized per [R:present]; font upload via existing Uploader; UseDynamicRendering with swapchain format); `beginFrame()` (SDL event feed already flowing through Window — overlay hooks the existing event dispatch), `addPass(RenderGraph&, targetName)` — declares a graph pass (side-effect, reads nothing) whose callback renders draw data; core libs stay ImGui-free (only samples + rx_debug_ui link it).
**Steps:** GPU smoke test (overlay pass renders; readback shows non-empty overlay region with a forced demo window; zero validation errors) → implement → both presets → commit.

### Task 18: Shadow quality bridge (D21)

**Files:** Modify `shaders/multipass/` shadow path shared pieces as needed → but primary target is the Stage-2 scene shadow path: light ortho fitted to visible bounds (from DrawListBuilder), slope-scaled depth bias (vkCmdSetDepthBias on the shadow pass), 3×3 PCF in the standard lit path (`shaders/material/forward_entry.slang` shadow helper upgrade; sample 05 keeps its own simpler shaders untouched — documented). Reversed-Z main-camera migration lands here for the scene path (clear values, compare ops via PassSignature/pipeline state).
**Steps:** GPU test: acne scene (large ground plane at grazing light) renders without acne (probe variance check) and without peter-panning (contact probe); PCF softness probe (edge gradient spans ≥2 texels) → implement → commit.

### Task 19: Sample 09_scene + stress-v2 + release prep

**Files:** Create `samples/09_scene/` — loads DamagedHelmet field (headless: instanced helmets grid; present: `--scene sponza` when fetched) through Registry→Scene→DrawListBuilder→graph; fly-through camera (mouse capture + gamepad, D16 input); ImGui HUD: FPS/frame-ms, cull counters, vsync toggle, layer-mask toggles (hide/show instance groups), light-channel demo toggle, pool stats; stress-v2 mode `--stress` (same 30k-draw workload as sample 07 but through the full scene path — publishes A/B numbers vs 07 in the report + release notes); headless gate: counter assertions + tolerance pixels; MANUAL_VERIFICATION rows; packaging/CI; README/roadmap updates; `docs/superpowers/specs` layer table tick for layer 8.
**Steps:** TDD gate → implement → numbers (desktop; Deck rows added to MANUAL_VERIFICATION as unchecked) → packaging → commit(s).

---

## Execution notes (coordinator)

- Models: Tasks 8, 16 Haiku (mechanical, fully specified); all others Sonnet; reviews all Sonnet; final whole-phase review at phase end.
- Sequencing: Stage 0: T1→T2→T3 sequential (T3 zones touch files broadly); T4/T5/T6 parallelizable in worktrees after T3 (disjoint); T7 after T2+T6; T8 anytime after T4 (Haiku, disjoint files). Stage 1: T9→T10→T11→T12→T13 (T11 parallelizable with T12 if worktree-disjoint — coordinator judges at dispatch). Stage 2: T14→T15→{T16,T17 parallel}→T18→T19.
- Each stage ends with a coordinator checkpoint: suite green both presets, stage sample packaged and run standalone, numbers recorded in ledger, board cards moved, then next stage dispatches.
- Phase exit: final whole-phase review (fresh, most scrutiny on cross-stage seams: registry↔scene handle lifetimes, parallel recording under real scene loads, threading contract adherence), one fix wave, push, CI green, tag v0.4.0-phase4, release with both packages + published numbers, board cards closed.
