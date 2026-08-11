# Phase 4: Scene & Assets — Design

**Date:** 2026-08-11
**Status:** Approved for implementation
**Author's note:** gap analysis, stage architecture, and every decision in
this document are the coordinator's own; the four research files in
`.superpowers/sdd/2026-08-11-phase4-scene-assets/` (cited as [R:assets],
[R:threading], [R:scene], [R:present]) supplied version-specific facts
only. Inputs binding on this spec: `2026-08-10-phase4-seed-notes.md`
(15 committed items), the master registry
(`2026-08-09-toolchain-platform-rhi-design.md`), CLAUDE.md's performance
exit-criterion policy, and the Phase 3 ledger's carried items.

## Goal

Turn the Phase 1-3 machinery into a renderer that eats real content: a
three-stage phase delivering (0) the engineering debt and threading
foundations owed by earlier phases, (1) a production asset pipeline
(glTF 2.0 + KTX2 + meshoptimizer + PBR materials), and (2) the scene
layer (render proxies, parallel culled draw-list building, layer masks)
— exiting with three new samples (07_stress, 08_gltf_viewer, 09_scene),
published performance numbers, and release v0.4.0-phase4.

## Gap analysis — what the pre-spec conception missed

The seed notes and board captured 15 commitments; auditing them against
what a production-grade renderer requires surfaced these additional
items, all now specified (decision references in parentheses):

| # | Gap | Consequence if unaddressed | Resolved by |
|---|-----|---------------------------|-------------|
| G1 | No draw sorting anywhere in the plan | Alpha-blended glTF content renders wrong (needs back-to-front); opaque overdraw unmanaged | D14 sort keys |
| G2 | No tangents in the vertex model | Normal-mapped PBR impossible | D8 vertex format, MikkTSpace |
| G3 | Depth precision never decided | Standard-Z z-fighting on real scene scales; expensive to change later | D13 reversed-Z |
| G4 | No thread-safety contracts on Phase 2/3 subsystems | Async asset loading corrupts BindlessTable/Uploader state | D5 threading contract |
| G5 | Pixel gates assume exact equality | Real content + filtering diverges across drivers; CI gates flake or lie | D17 tolerance gates |
| G6 | No sampler model | glTF wrap/filter modes ignored; no anisotropy | D10 sampler cache |
| G7 | sRGB vs linear per texture role undecided | Washed-out or crushed rendering (classic gamma bug) | D10 format-by-role |
| G8 | Multi-primitive meshes unmodeled | glTF meshes with per-primitive materials can't import | D7 submesh model |
| G9 | No asset ownership model | Leaks or double-frees between scene and import | D6 asset registry |
| G10 | Transform hierarchy scope undecided | Either scope-creep (runtime hierarchy) or silent inability to import nested scenes | D12 flatten-at-import |
| G11 | No bounds pipeline | Culling has nothing to test against | D7 import-time AABBs |
| G12 | No fallback/error assets | Missing texture = crash or garbage instead of obvious magenta | D11 fallbacks |
| G13 | No memory budget/growth policy for pools | First big scene = mysterious failure | D9 pool policy |
| G14 | CI content strategy undefined | Either bloated repo or unfetchable tests | D16 asset strategy |
| G15 | Wall-clock CI gating is noise on shared runners (>30% variance [R:present]) | Perf gate flakes or gets ignored | D18 counter gating |

## Fixed decisions

### D1 — Stage architecture: debt first, then assets, then scene

Three stages, each with an exit sample and its own reviewable
deliverables. **Stage 0 (Foundations & Debt)** clears everything owed by
Phases 1-3 before new scope opens — user-directed. **Stage 1 (Asset
Pipeline)** builds import; **Stage 2 (Scene & Culling)** builds the
layer that consumes it. Rationale for pool-before-import (Stage 1 order):
imported geometry must land directly in the pooled fast layout — never a
per-mesh-buffers version first (retrofit bar per CLAUDE.md performance
policy).

### D2 — Task scheduler: enkiTS v1.12 (zlib)

Single scheduler for the whole engine: **enkiTS** [R:threading].
`TaskSet` (work-stealing parallel-for, zero-allocation) maps to culling
and recording fan-out; `IPinnedTask` maps to the dedicated IO thread and
main-thread-marshalled GPU handoffs; external-thread participation lets
the main thread pump work. The courier findings hedged toward
Taskflow-plus-enkiTS; **overruled**: one scheduler only (two thread pools
fight over cores), we have no DAG-shaped workloads, Taskflow's pinning
support is unverified [R:threading], and enkiTS's footprint suits
middleware. Taskflow is the recorded alternative if graph-shaped
scheduling needs ever materialize. Wrapped thinly in `src/rx_task`
(`rx::task::Scheduler`) so the dependency stays swappable.

### D3 — Profiling: Tracy (client ≥ v0.11) from Stage 0

CPU zones on every frame-path and asset-path function that matters; GPU
zones via `TracyVkContextCalibrated` where `VK_EXT_calibrated_timestamps`
exists, plain `TracyVkContext` fallback otherwise (lavapipe support
unverified [R:threading] — the integration task must verify and guard).
`TRACY_ENABLE` is a CMake option, ON in dev presets; the client is
passive (~no cost) until a profiler connects. All performance claims in
reports from Stage 0 onward cite Tracy captures or CI counters — never
prose estimates (CLAUDE.md policy).

### D4 — Parallel command recording: secondary command buffers with dynamic-rendering inheritance

Per-thread × per-frame-in-flight command pools, reset as whole pools
each frame. Within a graphics pass, the pass's draw work is recorded
into secondary command buffers on enkiTS workers using
`VkCommandBufferInheritanceRenderingInfo` (core 1.3) [R:threading], and
the primary executes them inside its `vkCmdBeginRendering` scope with
`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`. Chosen over
multi-primary stitching because our workload is one heavy pass
(intra-pass chunking), and this is the shipping pattern (Godot 4.3
reference [R:threading]).

**Parallelism is the engine default, not a mode (user-directed):** there
is no on/off switch and no caller-chosen chunk count. A pass provides
either a whole-pass callback (hand-written simple passes — the library
model means the engine cannot split code it does not own) or a chunked
callback; every chunked pass records in parallel unconditionally, with
the executor deriving chunk count from the scheduler and grain-based
scaling making small workloads effectively serial at the cost of one
task submission — self-scaling, never toggled. All engine-owned work
(culling, draw-list building, import internals, and Stage 2's
scene-submit helper, which is the chunked callback for any scene-driven
pass) is parallel by default with no flags. `--threads` exists only in
the stress benchmark as a measurement instrument.

**Chunk-0 main-thread guarantee (adjudicated 2026-08-11, Task 7):**
chunk 0 of every chunked pass records synchronously on the main thread,
before the worker fan-out; chunks >= 1 record on workers. This is a
deliberate affordance: main-thread-only APIs (per docs/threading.md's
D5 list, e.g. MaterialSystem::bindInstance) are legal in chunk 0 and
ONLY chunk 0. Calling them from chunk >= 1 is a contract violation that
must fail loudly in dev builds — main-thread-only subsystems carry
debug thread-affinity assertions (dev-preset-active regardless of
NDEBUG). The alternative (splitting bind APIs into main-side resolve +
any-thread record) is the recorded future path if profiling ever shows
chunk 0 serialization dominating.

### D5 — Threading contract for existing subsystems (Phase 4 rule)

**GPU-object mutation stays main-thread-only**: BindlessTable
registration, Uploader submissions, MaterialSystem load/getPipeline,
DeletionQueue — none grow locks in Phase 4. Workers do pure CPU work
(parse, decode, transcode, optimize, cull, record into their own pools)
and hand results to the main thread via `IPinnedTask` completion
queues. Rationale: retrofitting concurrency into reviewed Phase 2/3
subsystems is high-risk, low-need — the handoff pattern serves asset
loading fully; truly concurrent GPU-object creation is a streaming-phase
design. The contract is documented in `docs/threading.md` (Stage 0
deliverable) and every touched header gets a one-line thread-affinity
note.

### D6 — Asset model: `rx_asset` registry with handle-owned assets

`src/rx_asset` owns import and asset lifetime: `MeshAsset` (submeshes →
geometry-pool ranges + AABBs + preserved skin data), `TextureAsset`
(bindless index + dims + format), `MaterialAsset` (StandardPBR parameter
set + texture references). Generational handles (rx_core pattern);
assets are registry-owned, released explicitly or at registry teardown —
no refcounting in Phase 4 (scenes outlive frames; the registry outlives
scenes; streaming-phase revisits). Scene proxies reference assets by
handle; dangling handles fail loudly (generational check).

### D7 — Import pipeline: fastgltf → MikkTSpace → meshoptimizer → pool

**fastgltf** (current release [R:assets]; 5-7× faster than cgltf,
full KHR extension surface incl. KHR_texture_basisu). Per mesh
primitive (submesh): positions/normals/uv0 mandatory, tangents taken
from the file or generated with **MikkTSpace** (the industry-standard
reference implementation; vendored, license verified at vendoring —
G2); then the canonical meshoptimizer sequence
(generateVertexRemap → remap → optimizeVertexCache → optimizeOverdraw →
optimizeVertexFetch [R:assets]); AABB computed from final positions
(G11); skinning data (JOINTS_0/WEIGHTS_0, skins, inverse bind matrices)
parsed and PRESERVED in `MeshAsset` per seed item 14, unused until the
animation phase. **Amended per feature-gap audit FG2:** glTF punctual
lights (KHR_lights_punctual) and cameras are likewise parsed and
PRESERVED in `ImportedScene` (types/params stored, unconsumed until the
techniques phase — same retrofit economics as skinning). COLOR_0 and
TEXCOORD_1 are explicitly deferred (recorded, not silently dropped:
importer logs when present).

### D8 — Vertex format (pooled, fixed for Phase 4)

One interleaved format for all pooled static geometry:
`position f32x3 | normal f32x3 | tangent f32x4 (w = handedness) |
uv0 f32x2` = 48 bytes, plus u32 indices. Chosen over packed encodings
(1010102 normals etc.) for Phase 4 simplicity; packing is a recorded
optimization candidate once meshlets arrive. Existing procedural samples
keep their own layouts (unchanged); the pool serves imported content.

### D9 — Geometry pool: single VB/IB pair suballocated with VMA's virtual allocator

`rx::asset::GeometryPool`: one big vertex buffer + one big index buffer
(DEVICE_LOCAL), suballocated with **VmaVirtualBlock** — VMA's CPU-side
virtual allocation API, already vendored, TLSF-backed, built for exactly
this. **Overrules** the courier suggestion of OffsetAllocator
[R:scene]: equivalent algorithm, but VMA is already in-tree, reviewed,
and cross-compiled — a new dependency must beat the incumbent, not tie
it. Draws use firstIndex/vertexOffset against the shared buffers (the
GPU-driven-ready layout, CLAUDE.md policy). Growth: fixed-size chunks
(default 64 MB vertex / 32 MB index, configurable), new chunk on
exhaustion (multiple pool blocks, each own VB/IB — draw records carry
the block id), no defragmentation in Phase 4 (recorded). Budget stats
surfaced via Tracy plots and the ImGui HUD (G13).

### D10 — Textures: KTX2-first with role-typed formats and a sampler cache

**libktx** (current release [R:assets]) parses KTX2 and transcodes
Basis to block formats CPU-side; upload stays ours (existing Uploader +
BindlessTable). Format by role (G7): baseColor/emissive → BC7_SRGB;
normal → BC5_UNORM (two-channel, Z reconstructed in shader);
metallic-roughness/occlusion → BC7_UNORM (or BC4 for single-channel
occlusion). Mip chains come from the container; if a KTX2 lacks mips
the importer warns (offline `toktx` guidance in docs — no runtime mip
generation for compressed formats). Non-KTX2 sources (PNG/JPG via
already-vendored stb) upload as RGBA8_(SRGB|UNORM) with an importer
warning recommending KTX2 conversion — no runtime block compression in
Phase 4 (recorded). **Sampler cache** (G6): glTF sampler → VkSampler
deduplicated by state; anisotropy default 8× when the device supports
it; wrap modes honored.

### D11 — Fallback assets (G12)

Registry-owned defaults created at init: 4×4 magenta/black checkerboard
(missing/failed texture), 1×1 white + flat-normal + neutral-MR
utility textures (unbound material slots), and an error material
(unlit magenta). Import failures log via the public sink and bind
fallbacks — a bad asset must be *visible and named*, never a crash and
never silently absent.

### D12 — Hierarchy: flattened at import (G10)

glTF node trees are walked once at import; world transforms bake into
per-instance transforms; the scene layer stores flat transforms only.
Runtime hierarchy/re-parenting arrives with the animation phase (where
it is actually needed). This bounds Phase 4's TransformManager to the
performance-critical flat SoA case (seed item 9c), including the
prev-frame slot layout committed for the temporal cluster.

### D13 — Reversed-Z for the main camera (G3)

Camera projection uses reversed-Z (near=1, far=0), depth attachment
cleared to 0.0, compare op GREATER_OR_EQUAL — decided now because the
scene camera is where depth conventions crystallize, and migrating
later touches every pipeline and probe. Shadow maps keep standard-Z in
Phase 4 (ortho depth is less precision-critical; bias tuning is
calibrated for it; the cascades work in the techniques phase revisits).
`rx::scene::Camera` owns the projection helpers so samples cannot get
it inconsistently wrong.

### D14 — Draw lists with sort keys (G1)

`DrawListBuilder` produces per-view lists of draw records
(submesh range, material instance, transform index, block id). Two
partitions per view: **opaque** sorted front-to-back by
(pipeline, material, depth-bucket) packed in a u64 key (state-change
minimization + early-Z); **blend** sorted strictly back-to-front by
depth. Alpha-MASK draws go in the opaque partition (their pipeline
variant handles cutoff). Sorting is `std::sort` on u64 keys per
partition, parallelized per-view via TaskSet when list size warrants.

### D15 — Culling in Phase 4: frustum + shadow-caster, CPU, parallel

Frustum culling: camera planes extracted from the (reversed-Z)
view-proj; AABB vs 6 planes, batched over SoA bounds in TaskSet chunks
[R:scene niagara precedent for data layout; GPU culling stays deferred
to the GPU-driven milestone]. Shadow-caster culling: directional light
gets an ortho frustum fitted to the camera frustum's world bounds, with
casters extruded conservatively along the light direction so
off-screen casters still cast (the classic correctness trap [R:scene]).
Per-view counters (visible/culled/casters) feed the HUD and the CI
counter gates. Layer masks (seed 5): renderable `layers: u32` vs camera
`cullMask: u32`; light `channels: u8` vs renderable `channels: u8`
filter both lighting and shadow-caster lists; defaults all-ones
(conventions per [R:scene]: 32-bit layers à la Unity/Godot; 8 channels
is deliberately more generous than Unreal's 3).

### D16 — Test content strategy (G14)

Committed to the repo: one tiny hand-authored .gltf (a textured cube,
<20 KB) for unit tests. Fetched with checksums (slang-prebuilt
pattern, CI-cached): **DamagedHelmet** (~small, the standard PBR
correctness asset) for gates, **Sponza** for local/present-mode wow
[R:assets — both from Khronos glTF-Sample-Assets, CC licenses recorded
in the fetch script]. CI never downloads Sponza.

### D17 — Pixel gates on real content: tolerance-based (G5)

Content gates (sample 08/09 headless) compare against
committed reference PNGs rendered on lavapipe at 256×256: per-channel
tolerance (±4/255) with a failing-pixel budget (<0.5%), plus the
existing analytic probes where applicable. References are
lavapipe-only (CI's driver); local GPU runs report divergence as info,
not failure. Reference regeneration is an explicit documented script,
never automatic.

### D18 — Performance gating: counters gate, wall-clock trends (G15)

CI perf gates assert **deterministic counters** (draws submitted,
draws culled, instances, pool bytes, descriptor allocations) against
committed expectations — exact and noise-free. Wall-clock/Tracy numbers
are published as artifacts and in release notes from the dev machine
(and Deck when available), trend-tracked but never CI-blocking
(>30% shared-runner variance [R:present]). This implements CLAUDE.md's
"performance regression blocks like a failing test" via the numbers
that *can* block honestly.

### D19 — Scene data model: render proxies via component managers (seed 11 resolved)

No ECS. `src/rx_scene` implements Filament-precedent managers
[R:scene]: `RenderableManager`, `TransformManager`, `LightManager`,
plus a plain `Camera` value type — SoA storage, generational handles,
create/set/destroy API, consumed by DrawListBuilder. EnTT remains the
recorded candidate for samples/tooling only. (Registry updated.)

### D20 — Debug overlay: Dear ImGui v1.92.x as `rx_debug_ui` (seed 12 confirmed)

Upstream SDL3 + Vulkan backends with `UseDynamicRendering` [R:present];
rendered as a normal declared graph pass writing the backbuffer; own
descriptor pool per frames-in-flight; font upload through existing
Uploader. Core libraries and the public ABI stay ImGui-free (hard
boundary restated). Docking excluded (tooling phase).

### D21 — Shadow quality bridge (Stage 2)

Sample 05's fixed-bias single tap will not survive Sponza. Phase 4
ships the production-credible single-map baseline: light frustum fitted
to the visible scene, slope-scaled depth bias, 3×3 PCF. Cascades remain
the techniques-phase item (registry unchanged).

### D22 — Materials: StandardPBR + Unlit with alpha modes

StandardPBR implements glTF metallic-roughness core: baseColor(+tex),
metallic/roughness(+tex), normal map (BC5 reconstruction), occlusion,
emissive, alphaMode OPAQUE/MASK (cutoff in shader)/BLEND (pipeline
variant: blending on, depth-write off, cull off), doubleSided → cull
variant. Manual exposure parameter on the tonemap (G-item; auto-exposure
is techniques-phase). Both materials are ordinary `.slang` modules on
the public `IMaterialShader` interface — zero special treatment (seed 3
restated). Specialization bits get their first real axes (alpha mode,
double-sided). **Amended per feature-gap audit FG1:** StandardPBR
includes an interim flat ambient/environment term (uniform color ×
occlusion — metals must not render black without IBL); the full
skybox+IBL environment path is registered for the techniques phase.

### D23 — Public ABI in Phase 4

Grows by exactly one entry point: the log sink (seed 13). Scene, asset,
and task APIs are internal C++ this phase; their ABI projection is
SDK-phase work. (Materials ABI evolution from Stage 0 texture wiring
regenerates GUIDs per the documented pre-release policy.)

## Stage exit criteria

**Stage 0 — Foundations & Debt.** History resources + bounds check;
enkiTS + threading contract doc; Tracy; texture-sampling wiring; log
sink; vsync control in all samples; parallel recording + **sample
07_stress** (≥30k procedural instanced draws, `--threads N`,
`--vsync`, Tracy-zoned, counter gate + published parallel-vs-single
numbers); ledgered-minors cleanup batch. All Phase 1-3 debt is either
closed or explicitly re-recorded with rationale. **Exit gate
(user-mandated): a Fable-model adversarial audit of the entire existing
foundation, with every finding closed or explicitly ruled before the
stage completes** — see the plan's Stage 0 exit gate section.

**Stage 1 — Asset Pipeline.** GeometryPool; import
(fastgltf+MikkTSpace+meshopt, skin preservation, AABBs, fallbacks);
KTX2 textures + sampler cache; async load (workers + main-thread
handoff, Tracy-evidenced); StandardPBR/Unlit; **sample 08_gltf_viewer**
(DamagedHelmet, orbit camera, tolerance pixel gate, import-time stats
logged).

**Stage 2 — Scene & Culling.** Proxies (D19) with layers/channels;
parallel DrawListBuilder with sort keys + frustum & shadow-caster
culling + counters; input expansion; rx_debug_ui; shadow bridge;
**sample 09_scene** (fly-through with mouse+gamepad, HUD with culling
counters and toggles, layer/channel demo, stress-v2 numbers through the
scene path A/B'd against sample 07's direct path). Both presets green,
zero validation errors (sync validation active), packaged samples,
release **v0.4.0-phase4** with published numbers.

## Deferred (not dropped) — Phase 4 additions

Async compute execution and intra-frame aliasing (unchanged, techniques
phase — aliasing now correctly sequenced after Stage 0's history
resources per the registry constraint); vertex packing; runtime
hierarchy (animation phase); refcounted/streamed assets, concurrent
GPU-object creation (streaming phase); COLOR_0/TEXCOORD_1; runtime
block compression; auto-exposure; cascaded shadows; GPU culling;
pool defragmentation; Filament-style variant filters. LICENSE file
remains a user decision.
