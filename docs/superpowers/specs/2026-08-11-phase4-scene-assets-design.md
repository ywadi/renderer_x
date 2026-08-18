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
**Gate annotations (2026-08-18, matrix-issue06 verified against bgfx
SortKey + Filament CommandKey):** the key's bit layout is documented
with named constants + a decode() round-trip test; the depth bucket is
derived by truncating the monotonic float32 bit pattern (reversed-Z
depths are non-negative, so the raw IEEE-754 pattern is
sort-order-preserving) — never a linear rescale; the low bits carry a
deterministic tie-break (the renderable's stable creation index) so
`std::sort`'s output is fully determined at equal keys; partition
sort DIRECTIONS are asserted on one shared fixture (opaque
depth-bucket decreasing, blend increasing, under reversed-Z — the
sign-flip bug class Filament's own `~distance` negation exists for);
a `priority` tier (u8, 0-7, default 4 — Filament precedent) sits
above pipeline bits; per-primitive blendOrder bits are RESERVED but
unpopulated (techniques phase, registry).

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
**Gate annotations (2026-08-18):** the coupled channel model
(channels gate lighting AND shadow-casting) is Unreal's, and is a
DELIBERATE divergence from D19's Filament precedent, whose channels
gate lighting only (verified against Filament shader source,
matrix-issue07) — one predictable knob while the only light is
directional; the separable split is revisited with punctual lights
(techniques phase). All-ones defaults likewise diverge from every
surveyed precedent (all default to bit-0-only) — deliberate
middleware opt-out semantics; the default-visible/default-lit test is
the regression guard. `RenderableDesc` additionally carries
`castsShadows: bool = true` (per-object caster opt-out, Filament
precedent). BLEND-partition draws are excluded from shadow lists in
Phase 4, and the depth-only caster pass has no alpha test (MASK
casters cast full silhouettes) — both documented, tested limitations
revisited with the cascades work.

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
**Amended (gate ruling RC6, 2026-08-18):** wall-clock STALL DETECTORS
may block CI — an assertion whose threshold sits an order of magnitude
above runner noise and whose failure signature is a correctness bug
(e.g. an unbounded per-frame fence wait: tens of ms vs a 10 ms
detector threshold) is a correctness gate, not a perf trend. Perf
TRENDS remain never-CI-blocking. Concretely: the async-import overlap
test asserts a 2 ms per-call main-thread budget locally (published,
trend-tracked) and a 10 ms per-call stall-detector ceiling in CI
(blocking).

### D19 — Scene data model: render proxies via component managers (seed 11 resolved)

No ECS. `src/rx_scene` implements Filament-precedent managers
[R:scene]: `RenderableManager`, `TransformManager`, `LightManager`,
plus a plain `Camera` value type — SoA storage, generational handles,
create/set/destroy API, consumed by DrawListBuilder. **Consumer-boundary
contract:** the handle API is the seam a HOST engine drives — a handle is
a plain value the host stores inside its own world model (ECS component /
scene-graph node / flat array; the renderer is neutral), and the host's
systems call set-by-handle each frame. The internal SoA managers are
ECS-shaped storage, not an ECS framework, never exposed as one. Design
for cheap per-frame set-by-handle from host systems. EnTT remains the
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

### D24 — Renderer-wide memory budget & eviction invariant (FG9 elevation, card #27)

Added 2026-08-18; carries the 2026-08-12 registry elevation and ledger
rulings into the phase spec proper (the elevation previously lived only
in the master registry + SDD ledger). Phase 4 delivers the memory-USAGE
management foundation: (a) allocation **accounting** — every VMA
allocation attributed to a category (geometry pool, textures, transients,
staging, internal); (b) `VK_EXT_memory_budget` enablement
(`VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` + opportunistic device
extension) with `vmaGetHeapBudgets` polling; (c) a host-facing POD
memory report (per-category bytes, heap budget/usage) feeding HUD +
Tracy plots; (d) `VK_ERROR_OUT_OF_DEVICE_MEMORY` handling at every
allocation site — today there is zero handling anywhere (verified
2026-08-18); loud, named failure with the report attached, never a
crash; (e) the **eviction INVARIANT** built and tested: residency-
tolerant handle resolve (a resolve may yield the fallback while
nonresident), handle-mediated references everywhere (no raw
pointer/index escapes that break under eviction), and one working
deferred-eviction path proving the system tolerates it. The eviction
**POLICY** (automatic what/when, streaming-in) remains streaming-phase,
cheap later ONLY because the invariant lands now. Import/registry and
DrawListBuilder tasks implement the invariant; the primary gate enforces
it in acceptance criteria. The Deck's UMA makes over-commit a hard
failure, not a slowdown — this is floor-hardware work, not polish.

### D25 — Upload completion tickets: non-blocking flush invariant (card #28)

`Uploader::flush()` today ends in an unconditional
`vkWaitForFences(..., UINT64_MAX)` (upload.cpp:278, "synchronous by
design" — an acceptable Phase 2 choice that Stage 1 would fossilize into
every asset-pipeline entry point). Phase 4 changes the **contract**, not
the queue architecture: `flush()` returns a pollable `UploadTicket`;
`isComplete(ticket)` / `wait(ticket)` added; staging-ring reclamation
keys off ticket completion instead of having-already-blocked.
**Primitive (gate ruling RC4, 2026-08-18): one TIMELINE SEMAPHORE**
owned by the Uploader — each work-submitting `flush()` signals a
monotonically increasing value; `UploadTicket = {uint64 value, uint32
ringGeneration}`; `isComplete` = one `vkGetSemaphoreCounterValue`
compare, `wait` = `vkWaitSemaphores`. Chosen over a fence pool because
the current single-reused-fence design is structurally incompatible
with pollable tickets (reset-while-referenced hazard, matrix-issue28
row 1); a monotonic counter eliminates the hazard by construction
(core Vulkan 1.2). Direct-path-only batches (UMA/ReBAR — the Deck
common case) return an already-complete ticket; ring reclamation
follows the D3D12 fence-value-queue pattern (poll-reclaim completed
entries first, block only on the oldest ticket covering the needed
range). All Stage-1 call sites (GeometryPool upload,
TextureCache load, importer, async import) consume tickets;
`MeshBuffers::create` keeps blocking behavior explicitly via
`wait(ticket)`, documented as a convenience. D5's threading deferral
(main-thread-only mutation, no locks) is orthogonal and unchanged —
the API stays main-thread-only. `Device::create` additionally acquires
an optional dedicated transfer queue when present (graphics fallback,
logged degrade, optionality principle) so the queue plumbing is prepaid;
actually USING it (cross-queue ownership transfer, multi-frame staging)
stays streaming-phase policy (registry). Async-import overlap tests
gain a wall-clock main-thread-block assertion alongside frame counters —
the counter-only criterion cannot detect a per-frame fence stall.

### D26 — GPU-driven readiness & submission fast path (invariants now, indirect later)

GPU-driven/indirect execution stays at the registered geometry-phase
milestone — but four invariants are Phase-4-cheap and retrofit-expensive,
so they bind Stage 1/2 designs now (CLAUDE.md fast-path-as-default):
1. **Per-draw addressing:** scene-path shaders receive per-draw data via
   `firstInstance`/`gl_InstanceIndex` indexing into a bindless storage
   buffer — never per-draw push constants (structurally impossible under
   indirect draw; sample 07's push-constant loop is the anti-pattern the
   scene path must not inherit). Binds StandardPBR/Unlit and the
   `recordDrawList` helper.
2. **Draw-list layout:** `ViewLists` stores geometry fields in a
   `VkDrawIndexedIndirectCommand`-compatible packed array with per-draw
   payload (material index, instance index) in a parallel array — SoA,
   GPU-uploadable, not an AoS record struct. Grouped by GeometryPool
   `blockId`; the per-block indirect submission granularity (one future
   MDI call per block) is the recorded, deliberate bound.
3. **Instancing collapse:** after sorting, runs of identical
   (pipeline, material, mesh range, block) collapse into instanced draws
   (`instanceCount > 1`) — resolving seed 9c's instancing/batching
   commitment that D14's sorting alone left half-delivered. Counters
   report records-in vs draws-submitted (CI-gateable).
4. **BDA enablement:** `bufferDeviceAddress` cannot be retrofitted —
   VMA requires the flag at allocator creation and buffers need the
   usage bit at creation, so Stage-1 pool blocks built without it would
   all be reallocated at the GPU-driven milestone. Enable
   opportunistically (never a device-selection requirement; logged
   degrade; lavapipe support verified in-task before CI relies on it):
   feature bit in `features12` when supported,
   `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`, usage bit on
   device-local buffers, `Device::supportsBufferDeviceAddress()`.
   Nothing consumes addresses in Phase 4 — enablement only.

### D27 — Main-thread pipeline pre-resolution before chunked fan-out

`MaterialSystem::getPipeline()`/`bindInstance()` are main-thread-only
and runtime-guarded; chunks ≥1 of a chunked pass run on workers — so the
planned `recordDrawList` helper cannot call them off chunk 0 (the sample
06 migration already hit this exact wall; "API split" was recorded as a
future path). Phase 4 resolution: the DrawListBuilder pass **pre-resolves
every distinct (material, pass-signature, specialization) pipeline and
per-draw parameter offsets on the main thread before fan-out** — nearly
free, because the sorted draw list already enumerates every distinct
pair before any command is recorded. Worker chunks then consume only
pre-resolved plain data (pipeline handles, offsets). This is a
correctness blocker for the scene path, and it simultaneously converts
first-use PSO creation into one deterministic, hoistable point — the
warmup hook the SDK/tooling-phase PSO-stutter item (registry) will
attach to. The resolve/record API split remains the recorded future
path; Phase 4 does not need it.

### D28 — Fixed-function pipeline-state axis on materials (gate ruling RC1, 2026-08-18)

Discovered independently by the #8 and #23 gate matrices:
`MaterialSystem::getPipeline()` hardcodes blend/cull/depth-compare
state with no cache-key axis, and `PassSignature`'s own header comment
disclaims carrying it — D22's BLEND/doubleSided variants are
unbuildable without new plumbing. Decision: `MaterialRecord` carries
the fixed-function state (alphaMode → blend enable + depth-write +
MASK-cutoff carriage; doubleSided → cull mode), included in the
pipeline cache key; `PipelineRequest` unchanged. alphaMode/doubleSided
are NOT specialization constants — they are `VkPipeline`
fixed-function fields with zero SPIR-V representation (the plan's
earlier "specialization bits gain alphaMode/doubleSided axes" phrasing
is corrected); MASK's cutoff is a per-instance uniform with an
always-present conditional discard. Lands with the material library
(first consumer); this is the reusable mechanism future
fixed-function-state needs (wireframe, stencil effects) extend.
Compare-op fork resolved as option (a): the scene-path shadow-caster
pass is a depth-only pipeline built OUTSIDE MaterialSystem, so
`getPipeline()` serves only reversed-Z main-camera pipelines from
Stage 2 on — no compare-op axis in Phase 4.

### D29 — Per-pass depth convention & clear values in the render graph (gate ruling RC2, 2026-08-18)

The executor's depth clear is a process-wide 1.0 constant at two
sites (executor.cpp:646, :1119) and `AttachmentDesc` has no
clear-value field — the scene path cannot clear a reversed-Z main
depth (0.0) and a standard-Z shadow map (1.0) in the same frame,
which D13 requires from Stage 2 on. Decision: `AttachmentDesc` gains
`DepthConvention { Standard, Reversed }` on depth attachments; the
clear value and the pass's expected compare direction derive from it;
the executor reads it at both sites. Lands with the shadow bridge
(its real blocker; that task's file list widens to rx_graph), tested
with a two-pass frame mixing both conventions.

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

**Stage 1 — Asset Pipeline.** Memory budget/accounting foundation +
eviction invariant contract (D24); upload tickets — non-blocking flush
+ optional transfer-queue acquisition (D25); GeometryPool with BDA
enablement (D26.4); import (fastgltf+MikkTSpace+meshopt, skin
preservation, AABBs, fallbacks, IO-source abstraction); KTX2 textures +
sampler cache; async load (workers + main-thread handoff,
Tracy-evidenced, wall-clock stall assertion per D25); StandardPBR/Unlit
(D26.1 addressing); window edge-state hardening (FG7); **sample
08_gltf_viewer** (DamagedHelmet, orbit camera, tolerance pixel gate,
import-time stats logged).

**Stage 2 — Scene & Culling.** Proxies (D19) with layers/channels;
parallel DrawListBuilder with sort keys + frustum & shadow-caster
culling + counters + indirect-ready layout/instancing collapse (D26) +
main-thread pipeline pre-resolution (D27) + caller-owned reused
draw-list storage; input expansion; rx_debug_ui; shadow bridge;
executor per-frame allocation elimination (steady-state zero-alloc
`execute()`); **sample 09_scene** (fly-through with mouse+gamepad, HUD with culling
counters and toggles, layer/channel demo, stress-v2 numbers through the
scene path A/B'd against sample 07's direct path). Both presets green,
zero validation errors (sync validation active), packaged samples,
release **v0.4.0-phase4** with published numbers.

## Deferred (not dropped) — Phase 4 additions

Async compute execution and intra-frame aliasing (unchanged, techniques
phase — aliasing now correctly sequenced after Stage 0's history
resources per the registry constraint); vertex packing; runtime
hierarchy (animation phase); refcounted/streamed assets, concurrent
GPU-object creation, transfer-queue upload policy + eviction POLICY
(streaming phase — the D24/D25 invariants make these additive);
COLOR_0/TEXCOORD_1; runtime block compression; auto-exposure; cascaded
shadows; GPU culling + indirect execution (geometry phase — D26
invariants prepaid); compute PSO creation/dispatch capability (geometry
phase, registry 2026-08-18); GPU particles (techniques phase, registry
2026-08-18); PSO warmup UX (SDK/tooling phase, registry 2026-08-18 —
supersedes the FG-audit near-miss ruling; D27 provides its hook);
pool defragmentation; Filament-style variant filters. LICENSE file
remains a user decision.
