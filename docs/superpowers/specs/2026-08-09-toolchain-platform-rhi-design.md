# RendererX: Toolchain + Platform + RHI — Design

Status: Approved
Date: 2026-08-09

## Context: overall renderer scope

RendererX is a ground-up renderer for third-party game engines, shipped as a
single DLL. Fixed constraints for the whole project:

1. Vulkan is the only GPU backend (cross-platform via one API, no D3D12/Metal
   backends).
2. The build must be able to compile from any host machine to any target
   platform (true cross-compilation, not per-OS build machines).
3. Shaders are authored in Slang. The public API exposes engine-facing
   abstractions (`IMaterial` and friends) that support extension,
   inheritance, override, and overload.
4. Prefer ready-made libraries or ported implementations over writing
   subsystems from scratch.
5. SDL3 is the windowing/input/platform library.

The full system decomposes into these layers, bottom to top:

| # | Layer | Responsibility |
|---|-------|-----------------|
| 0 | Toolchain / Cross-Build | Compile from any host to any target |
| 1 | Platform Abstraction | Window, input, events, threads, file paths |
| 2 | Foundation / Core | Math, logging, memory, containers, job system |
| 3 | RHI — Vulkan Backend | Device/queue/swapchain, command buffers, pipelines, sync |
| 4 | Shader Compilation & Reflection | Slang → SPIR-V, reflection-driven layouts |
| 5 | Resource Management | Buffer/texture lifetime, staging, bindless tables |
| 6 | Render Graph | Declarative passes/resources, auto barriers, transient aliasing (delivered: Phase 3) |
| 7 | Material / Shading Abstraction | `IMaterial`, `IShaderModule`, `ITexture`, `IMesh` (delivered: Phase 3) |
| 8 | Scene Submission | Render items, transforms, cameras, lights; culling/LOD (delivered: Phase 4 — scene submission/culling; LOD remains deferred) |
| 9 | Rendering Techniques | Shadows, lighting, post stack, upscaling |
| 10 | Asset Import (offline) | Mesh/texture import & baking tooling |
| 11 | Public SDK / DLL Surface | The interface game devs link against |
| 12 | Tools / Debug / Profiling | Validation, GPU markers, profiling |

This spec covers only layers 0–3. Each remaining layer gets its own spec later.

Two constraints were explicitly relaxed for now and should be revisited if
they become a problem: the C++ ABI-stability question for the DLL boundary
(layers 7/11), and shaders are shipped precompiled rather than requiring
runtime Slang compilation in the shipped DLL.

## Scope of this spec

Sub-project 1: **Toolchain + Platform Abstraction + RHI**. Goal: cross-compile
from a Linux host to Windows and Linux (Steam Deck included), with a working
Vulkan RHI that can render a hardcoded triangle through dynamic rendering.

## Target platforms

- Windows (native Vulkan drivers)
- Linux, including Steam Deck/SteamOS — Deck runs Mesa RADV on an RDNA2 iGPU,
  so it's covered by the Linux target, not a separate backend. Its feature
  ceiling (no mesh shaders, no hardware ray tracing) is a constraint on the
  RHI's *baseline* feature set, not a reason for special-casing it.
- macOS (via MoltenVK) — deferred, not part of this sub-project.

## Architecture

- **Build system:** CMake + Ninja. `zig cc`/`zig c++` is the compiler,
  selected via a CMake toolchain file per target triple: `linux-native`,
  `windows-cross-zig` (Linux host → Windows target). `steamdeck` is an alias
  of `linux-native` plus a Deck feature-safety check. `macos-cross` is
  deferred.
- **Dependencies:** vendored at a pinned commit/tag (submodule or
  `FetchContent`, no floating branches). A CMake dependency-cache module
  keys a build on `(dependency pin, target triple, zig version)`; a cache
  hit links pre-built static libs + headers as `IMPORTED` targets with zero
  compilation; a miss builds once and stores the result in `.deps-cache/`
  (reusable across machines/CI, not just local-disk). Slang is the
  exception: it is never built from source — official prebuilt release
  binaries (`libslang` + `slangc`) are fetched per platform and linked
  directly, since building a full compiler on every setup is both slow and
  unnecessary.
- **Platform layer:** SDL3 owns window/input/events/threads. `volk` owns
  Vulkan function loading (no static link against a per-platform loader).
- **RHI layer:** a thin, explicit Vulkan 1.3 wrapper. `vk-bootstrap` handles
  instance/device/swapchain bootstrap; VMA handles GPU memory. Baseline
  features are **dynamic rendering + synchronization2** — available on
  Windows, Linux, and Steam Deck. No mesh shaders or hardware ray tracing in
  the baseline; those become optional, capability-queried extensions later.
  Bindless-first descriptor design from day one, even though full
  descriptor/resource management is a later sub-project — retrofitting
  bindless after the fact is expensive to redo.

## Components

| Component | Responsibility | Depends on |
|---|---|---|
| `rx_core` | Math (GLM), logging (spdlog), basic handles/containers | none renderer-specific |
| `rx_platform` | Window/input/events/threads | SDL3, `rx_core` |
| `rx_rhi_vk` | Device/swapchain/pipeline/resource wrapper | volk, vk-bootstrap, VMA, `rx_platform`, `rx_core` |
| build scaffolding | Toolchain files per triple, dependency-cache module, Slang prebuilt-fetch script | — |

## Build flow

1. `cmake --preset linux-native` (or `windows-cross-zig`) configures with the
   matching toolchain file.
2. The dependency-cache module resolves each pinned dependency against
   `.deps-cache/<hash>/`: hit → link cached artifacts; miss → build once,
   then populate the cache. The Slang-fetch script downloads and checksums
   prebuilt binaries into `third_party/slang-prebuilt/<platform>/`.
3. `cmake --build` compiles only `rx_core`, `rx_platform`, `rx_rhi_vk`, and a
   smoke-test app against the cached dependency artifacts.
4. **Runtime smoke test** (the concrete "done" signal for this sub-project):
   open an SDL3 window → create instance/device/swapchain via vk-bootstrap →
   allocate a VMA-backed vertex buffer for a hardcoded triangle using a
   precompiled SPIR-V shader (Slang integration is layer 4, a later
   sub-project) → record the draw via dynamic rendering → present.

## Error handling

- Debug builds auto-enable Vulkan validation layers via vk-bootstrap's debug
  messenger, routed into `rx_core`'s logger.
- `rx_rhi_vk` translates `VkResult` failures into a small set of
  engine-level error codes at API boundaries (device-lost, out-of-memory,
  swapchain out-of-date/suboptimal → resize) rather than leaking raw
  `VkResult` through the wrapper.
- A dependency-cache build failure fails loudly, naming the exact dependency
  and target triple. It never silently falls back to rebuilding everything.

## Testing / success criteria

- CI matrix: `linux-native` and `windows-cross-zig`, each producing the
  triangle smoke-test binary. Deck-safe feature checks run as part of the
  `linux-native` job.
- Manual acceptance: the triangle renders on real Windows, Linux, and Steam
  Deck hardware.
- Build-time acceptance: the first checkout may take longer (one-time
  dependency bootstrap). Every subsequent build with unchanged dependency
  pins finishes in **under 1 minute** on the reference dev machine.

## Deferred to later sub-project specs (not dropped)

Everything below is still planned and will get its own design spec later —
it is out of scope for *this* spec only, not out of scope for the project:

- Slang runtime compilation/reflection (layer 4)
- Descriptor/resource management beyond the bindless-friendly baseline
  (layer 5)
- Render graph, materials, scene submission, techniques, asset import,
  public SDK surface, tooling (layers 6–12) — including, explicitly, these
  subsystems so they aren't lost before those layers get specced:
  - **Geometry processing** (layer 8, scene submission): meshlet
    generation, virtual geometry, LOD management, skeletal mesh skinning,
    morph targets. **meshoptimizer is the committed library** for
    import-time optimization (Phase 4), meshlet building, and LOD
    simplification (decided 2026-08-10; see also the Phase 4 seed notes).
    Asset decompression stays CPU-side (KTX2+zstd on worker threads) —
    GPU decompression was evaluated and dropped 2026-08-10 (D3D12-bound
    ecosystem; no expected win on the Deck floor).
  - **Lighting infrastructure & spatial queries** (layer 9, techniques):
    acceleration structures (BVH for ray tracing), clustered/deferred
    lighting grids, shadow cascades, global illumination probes.
  - **Hardware ray tracing** (layer 9, techniques; committed 2026-08-10):
    an OPTIONAL device capability, never baseline — Steam Deck (the
    hardware floor) exposes VK_KHR_ray_query/ray_tracing_pipeline via RADV
    but with minimal RT hardware, so every RT feature ships behind a
    startup capability query with a raster fallback (RT shadows → shadow
    maps, RT reflections → screen-space/cubemap). Optionality-with-
    fallback is the engine-wide principle for any feature above the
    Vulkan 1.3 baseline. Sequencing: requires Phase 4's scene layer
    (acceleration structures need real scene geometry); first deliverables
    are ray_query-based shadows/AO, not full RT pipelines. The existing
    foundations were chosen RT-compatible deliberately: bindless set 0
    (any-hit material access), Slang (RT shader stages), render graph
    (RT pass = compute-class pass + acceleration-structure resource type),
    VMA (AS allocation).
  - **Post-processing & image reconstruction** (layer 9, techniques): tone
    mapping, color grading, temporal anti-aliasing, motion blur, hardware
    upscaling wrappers (DLSS, FSR, XeSS). Shared infrastructure to build
    ONCE for this cluster (recorded 2026-08-10): per-pixel velocity/motion
    vectors (requires previous-frame transforms plumbed from scene
    submission — Phase 4's transform pools keep last-frame copies cheap)
    and render-graph HISTORY resources (persistent named images with
    load-instead-of-discard semantics — a deliberate extension of the
    graph's discard-per-frame transient model; pulled into Phase 4 as a
    bounded core task, seed item 15). TAA, temporal upscalers, and motion
    blur all consume the same two pieces. SEQUENCING CONSTRAINT: any
    future intra-frame transient-aliasing allocator must treat
    persistent/history resources as a first-class non-aliasable class —
    aliasing must not be designed against a world where every resource
    discards per frame.
  - **Profiling & debug instrumentation** (layer 12, tooling): GPU markers
    (PIX/RenderDoc integration), debug line drawing, memory leak tracking,
    performance counters.
- **Feature-gap audit register (2026-08-11**, full evidence:
  `.superpowers/sdd/2026-08-11-phase4-scene-assets/feature-gap-audit.md`;
  none of these may be dropped without an explicit recorded ruling**):**
  - *V1-blocking:* (FG1) environment lighting — skybox pass + image-based
    ambient/IBL (interim flat ambient term lands with StandardPBR in Phase 4
    Stage 1; skybox+prefiltered IBL at the head of the techniques phase);
    (FG2) punctual lights — point/spot types, attenuation/units, their
    shadow paths, clustered shading (techniques phase; glTF
    KHR_lights_punctual + camera parse-and-preserve lands in Phase 4
    Stage 1 import per the seed-14 economics); (FG3) dynamic content
    contract — runtime/transient meshes + per-frame texture updates for
    host UI/text/particles/video/procedural (bgfx transient/dynamic
    precedent; contract designed in the scene/SDK ABI projection —
    the SDK spec is OBLIGATED to answer it).
  - *V1-expected:* (FG4) device-lost/GPU-hang policy + crash diagnostics
    (breadcrumbs, VK_EXT_device_fault via the log sink; SDK policy +
    tooling phase); (FG5) host caps/degradation report + adapter
    enumeration/selection (SDK phase — the optionality principle's
    reporting channel; amended 2026-08-18: the same channel also
    carries a "present suspended" signal so hosts can observe the
    Phase 4 zero-extent/minimize suspend state); (FG6) MSAA policy decision + resolve-attachment
    semantics in the graph (decide in techniques-phase spec, before
    aliasing/history ossify the resource model); (FG7) window state —
    zero-extent/minimize guard + windowed/borderless-fullscreen toggle
    (Phase 4 Stage 1 hardening ticket #25; borderless is a plain SDL3
    window flag on the existing recreation machinery), occlusion + DPI
    policy (SDK phase at latest); (FG7b) EXCLUSIVE fullscreen via
    VK_EXT_full_screen_exclusive — an OPTIONAL capability (Windows-desktop
    only; irrelevant on the Deck floor where Gamescope owns the display),
    acquire/release + swapchain lifecycle, SDK/platform phase with a
    windowed fallback per the optionality principle; (FG8) HDR display
    output + swapchain colorspace ladder (techniques phase, with the
    post stack); (FG9) renderer-wide memory budget + reporting + eviction-policy design ELEVATED to Phase 4 Stage 1 (scheduled ticket, 2026-08-12; spec'd as Phase 4 D24, 2026-08-18) — accounting/VK_EXT_memory_budget/host report/eviction-contract land in Phase 4; the eviction MECHANISM + residency pairs with the streaming phase's workloads; (FG10)
    host-provided native window embedding via SDL3 foreign-window
    properties, or a recorded rejection (SDK spec must answer).
  - *Post-V1:* (FG11) consumer screenshot/capture API (SDK/tooling);
    (FG12) frames-in-flight configurability + present-wait latency
    control (profiling/SDK phase).
- macOS/MoltenVK support
- DLL ABI-stability strategy for the public interface
- **Task/mesh shaders** (committed 2026-08-10, geometry phase): optional
  fast path per the optionality principle — RADV exposes
  VK_EXT_mesh_shader on the Deck floor but RDNA2 task-stage throughput is
  weak and older GPUs lack it. The meshlet pipeline (meshoptimizer data)
  ships with a compute-culling + classic-vertex-pipeline baseline; mesh
  shaders are the capability-queried consumer of the same meshlet data.
- **GPU-driven pipelines / visibility buffers** (committed 2026-08-10):
  GPU-driven culling with indirect draw lists needs only core-1.3
  features (drawIndirectCount, device address, bindless) — no gating;
  sequenced with/after the geometry phase's meshlets and the HiZ
  occlusion milestone. Visibility-buffer shading (triangle-ID raster +
  deferred material resolve) is a techniques-phase decision AFTER
  meshlets exist; it requires a shade-from-ID path through the material
  system's specialization model — design work to scope in that phase's
  spec, not assumed.
- **Compute pipeline capability** (committed 2026-08-18, geometry phase,
  sequenced FIRST among that phase's items): compute PSO creation +
  dispatch API. The render graph's compute-class pass and barrier
  machinery are delivered (Phase 3; compute-class barrier stages derive
  from attachment-free signatures); the pipeline half is not —
  MaterialSystem is vertex+fragment-only by construction
  (kMaterialStageFlags) and getPipeline() rejects an attachment-free
  PassSignature outright, and the repo contains zero
  vkCreateComputePipelines/vkCmdDispatch (verified 2026-08-18). Required
  by three already-committed consumers: compute-culling as the
  mesh-shader baseline (above), compute pre-skinning (Phase 4 seed
  notes), and GPU-driven culling (above). Scope includes lifting the
  attachment-free-signature rejection. Deferral-safe by retrofit
  economics: a NEW code path, not a change at existing call sites.
- **GPU particles / compute-driven simulation** (committed 2026-08-18,
  techniques phase): simulation dispatch, per-frame particle buffers,
  sorted/instanced billboard submission. DISTINCT from FG3 — FG3 is the
  CPU-generated dynamic-content upload contract and does not cover
  GPU-resident simulation; before this entry, "particle" appeared
  nowhere in the planning corpus outside FG3 (registry hole found by the
  2026-08-18 claim validation). Depends on the compute pipeline
  capability above and on FG3's contract for the CPU-authored spawn
  path.
- **Upload/transfer asynchrony policy** (committed 2026-08-18, streaming
  phase): dedicated transfer-queue USE, queue-family ownership transfer
  for uploaded resources, multi-frame in-flight staging. The Phase 4
  invariant (D25: pollable UploadTicket from flush(), optional transfer
  queue ACQUIRED at device creation with graphics fallback) makes this
  additive. D5's Phase 4 threading deferral covers thread-safety only;
  the formerly unconditional fence wait in flush() is the D25 item, not
  this one.
- **PSO warmup UX** (committed 2026-08-18, SDK/tooling phase —
  SUPERSEDES the feature-gap audit's "near-miss" ruling on pipeline
  pre-caching): the disk-persistent VkPipelineCache is delivered and
  regression-tested, and Slang compilation happens at loadMaterial, not
  draw time — but vkCreateGraphicsPipelines still runs lazily on first
  (material, pass, specialization) use, on the main thread, with no
  warmup, so first-run/cold-cache hitches have no strategy. Scope:
  cold-cache warmup pass, background/parallel PSO creation (needs the
  recorded resolve/record API split — getPipeline is main-thread-
  guarded), and a host-facing "precompile these variants" API. Phase 4
  D27 (draw-list main-thread pre-resolution) provides the deterministic
  enumeration hook this attaches to. Fossilize/offline exhaustive
  packaging remains separately deferred (Phase 3 D7).
- **Primary-gate additions (2026-08-18)** — later-phase items surfaced
  by the Phase 4 ticket-completeness gate (evidence:
  `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/`; rulings in
  `gate/rulings-2026-08-18.md`):
  - 16-bit/mixed index sub-pools (geometry phase, with D8's vertex
    packing — bgfx's 16-bit-default precedent recorded; retrofit
    touches MeshRange + every draw-record consumer, so it travels with
    the other packing work).
  - Deck OOM-policy extensions (streaming phase, with eviction POLICY):
    `VK_AMD_memory_overallocation_behavior` (explicit
    fail-on-overcommit on AMD) and
    `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT` (proactive per-allocation
    budget enforcement) — both additive, both currently unnamed.
  - KHR_draco_mesh_compression: SUPPORTED (correction 2026-08-20 —
    this registry previously omitted it entirely): vendored Google
    Draco v1.5.7 decoder + fastgltf-native extension parsing landed
    with Phase 4 Task 13; hardened and acceptance-proven 2026-08-20
    (unique-id attribute resolution fix, bounds-validated reads,
    BoomBox integration + decoded-value assertions, issue #31).
  - KHR_meshopt_compression (Khronos ratification-track successor to
    EXT_meshopt_compression; NOT supported by fastgltf v0.9.0) — watch
    item on the fastgltf pin, re-checked at each dependency refresh; if
    gltfpack's default output migrates, the decode-to-open ruling
    silently drifts.
  - glTF `extras` host-preservation API (SDK phase, FG3-adjacent):
    hosts receive their own application JSON; fastgltf's
    setExtrasParseCallback makes it cheap; detection/logging already
    lands in Phase 4.
  - KHR_animation_pointer + KHR_interactivity — animation-phase watch
    line (the animation registry entry predates both).
  - Cubemap/array KTX2 loading (techniques phase, riding FG1 — the
    TextureCache-side work FG1's skybox/IBL consumer needs; Phase 4
    ships the log path only).
  - libktx v5 / UASTC-HDR (HDR texture INPUT, distinct from FG8's HDR
    display output) — watch item on the libktx v4.4.2 pin.
  - Async-import host surface (SDK/profiling phase): progress-API ABI
    projection (Phase 4's stage-enum+counts snapshot stays internal
    C++), host-tunable upload time-slice budget (FG12-adjacent; Unity's
    asyncUploadTimeSlice precedent); import priorities (streaming
    phase, with residency priorities). Phase 4 ships abandon-style
    cancellation; prioritized cancel goes here.
  - Byte-source IO routing for sub-resource reads (streaming/VFS
    phase, recorded 2026-08-18 from Task 15's adjudicated deviation):
    Phase 4 routes only the glTF DOCUMENT bytes through the pinned IO
    thread; buffer/image byte-source reads run on compute workers —
    zero practical risk for in-memory/filesystem sources, but a slow
    host byte source (VFS/network — the abstraction's purpose) would
    block compute workers on IO. When the streaming/VFS phase makes
    slow sources real, ALL byte-source reads move to IO-pool routing
    (background IO pool per Godot/Unreal precedent, not necessarily
    the single pinned thread). The debug thread-id assertion
    ("decode never runs on the IO thread") already guards the other
    half and carries forward.
  - Gamepad rumble/haptics/touchpad consumption (SDK/platform phase —
    per-call methods on the existing handle map, retrofit-safe); gyro
    consumption gated on SDL Deck support (libsdl-org/SDL#9148 watch:
    gyro/paddles undetectable on Deck at the SDL3 3.4.14 pin — Phase 4
    logs HasSensor + device identity instead).
  - Multi-gamepad host surface (`poll(JoystickID)`/enumeration, local
    co-op) — SDK phase; Phase 4's JoystickID-keyed internal map makes
    it additive.
  - Camera exposure API (techniques phase, with FG1 IBL/physical light
    units — Filament's camera-owned exposure model; Phase 4 keeps
    manual exposure on the tonemap per D22).
  - Per-primitive blendOrder sort tier (techniques phase, with the
    translucency work; Phase 4 documents reserved sort-key bits and
    delivers determinism via the creation-index tie-break instead).
  - Shadow-map resolution/format policy tiers (desktop/Deck) — the
    cascades-phase spec inherits Phase 4's explicit, parameterized
    1024/D32_SFLOAT default rather than archaeology through sample 05.
  - Dependency-boundary configure-time check — the CMake
    transitive-link-closure assertion landing with the ImGui module
    ("core libs stay ImGui-free") is recorded as the reusable pattern
    for every future layer-boundary claim.
- **Techniques-phase charter: advanced material & lighting renderer**
  (user-directed 2026-08-19; a CHARTER, not a frozen spec — the
  techniques-phase spec refines it, but the direction, sources, and
  priority order below are binding starting points; supersedes-by-
  consolidation the individual FG1/FG2/FG8 lines above, which remain
  valid and fold into this program). Objective: a serious modern
  material renderer, not a basic metallic+roughness+GGX implementation.
  - **Reference sources (per the port-don't-reinvent rule; licenses
    recorded at adoption):** Google **Filament** (Apache-2.0) as the
    canonical core-PBR source — Cook-Torrance/GGX/Smith with energy
    compensation for single-scattering (matters on rough metals),
    clearcoat, anisotropy, sheen/cloth, IBL, refraction/absorption,
    froxel-based clustered lighting, shadow techniques, HDR post.
    IMPORTANT: port from Filament's CURRENT `shaders/` implementation
    as canonical, never from its documentation prose — a clearcoat
    documentation discrepancy (identified June 2026) is resolved
    correctly only in the shader code. Khronos **glTF Sample Viewer**
    (Apache-2.0) as the material-vocabulary + reference-conformance
    source (its full extension set: clearcoat, sheen, anisotropy,
    specular, IOR, transmission, volume, dispersion, iridescence,
    diffuse transmission, emissive strength). NVIDIA **Falcor**
    (BSD-class; bundled NVIDIA SDKs like RTXDI/NRD/DLSS carry their
    own licenses — adopt Falcor patterns, not those SDKs, without a
    separate license decision) as the how-to-express-it-in-Slang
    reference. LTC area-light reference code from the original
    authors (permissive redistribution).
  - **Shader architecture:** ported PBR core organized as composable
    Slang modules (BRDF / StandardMaterial / ClearCoat / Sheen /
    Anisotropy / Transmission / IBL / Lighting / Shadows) with the
    lobe structure diffuse(Lambert) + specular(GGX/Smith+Fresnel) +
    clearcoat(GGX), each lobe fed by both direct lighting and IBL.
    The flagship material grows toward the full glTF-extension
    parameter set (baseColor, metallic, roughness, ior, specular,
    clearcoat+roughness, anisotropy, sheen, transmission, thickness,
    attenuationColor/Distance, dispersion, iridescence+thickness,
    diffuseTransmission, emissive) — with feature permutation via the
    existing specialization-bit system / Slang generics so materials
    only pay for the features they use (D28's axis + Phase-3 D8
    variant machinery are the prepaid seams).
  - **Glass is REAL transmission, never alpha blending:** transmissive
    BTDF keeping the Fresnel surface reflection; thin-surface mode
    (IOR/transmission/roughness/tint — windows, spectacles) and
    thick-volume mode (thickness map + Beer-Lambert absorption
    `T = exp(-absorption·distance)` via attenuationColor/Distance —
    bottles, liquids; per the Khronos thickness-approximation design
    for raster). Refraction samples a scene-color source:
    screen-space refraction first, environment/probe fallback on
    miss. **Frosted glass:** the opaque scene color renders into an
    HDR mip chain and transmission roughness selects the mip
    (sharp→blurred→frosted) — Filament's refractive-scatter model.
  - **Lighting: clustered Forward+** (Filament froxel reference — its
    compute-shader light-assignment is published; translate GLSL→
    Slang): camera-frustum froxels with per-cluster light lists;
    directional/point/spot/area at hundreds-to-thousands of local
    lights. Physical light intensities/units. **Area lights via LTC**
    (rect panels/screens/softboxes — the "suddenly looks AAA"
    feature). Punctual-light import consumption (KHR_lights_punctual,
    preserved since Phase 4) turns on here.
  - **Environment/indirect (at least as important as the BRDF):** HDR
    environment → SH (or irradiance-cubemap) diffuse + prefiltered
    specular cubemap with roughness-selected mips + BRDF-integration
    LUT; probes as the SSR fallback. SSR itself lands with the
    scene-color chain. GI proper stays the last step (existing
    layer-9 probes entry).
  - **Shadows:** sun = cascaded shadow maps, first-quality filter =
    **PCSS** (visible varying penumbra), EVSM later as the scalable
    alternative; spot = shadow atlas; point = cubemap or
    dual-paraboloid atlas; screen-space contact shadows. (Extends the
    existing cascades registry line with the technique ladder.)
  - **True volumetrics (committed 2026-08-19):** froxel-marched
    participating media (Frostbite/id-style volume fog), riding the
    SAME camera-frustum froxel grid the clustered light assignment
    already builds — per-froxel scattering/extinction accumulation
    fed by the clustered light lists (shadowed sun + local lights),
    temporal reprojection for stability, then a full-screen apply.
    Deliverables ladder: (a) screen-space radial god rays (cheap
    post pass, expressible against the Phase 3 graph today) as the
    entry tier, (b) froxel volume fog with shadowed directional
    in-scattering (the "physical god rays" tier), (c) local fog
    volumes/height fog fed by the same grid. Sequenced with/after
    clustered Forward+ (priority 3) since it consumes that
    infrastructure; exact priority slot decided at techniques-phase
    spec time.
  - **Frame pipeline target:** depth → shadows → clustered light
    assignment → opaque lighting → volumetrics (froxel march +
    apply) → SSR → scene-color mip chain → glass/transmission →
    particles/transparency → bloom → tone mapping (AgX/ACES-class,
    ties FG8 HDR output) → TAA.
  - **Priority order (binding):** (1) Filament-quality GGX PBR,
    (2) excellent HDR IBL, (3) physical light units + clustered
    Forward+, (4) good shadow filtering, (5) clearcoat + anisotropy,
    (6) real transmission/IOR/thickness glass, (7) SSR + probe
    fallback, (8) LTC area lights, (9) sheen/cloth, (10) iridescence
    + dispersion, (11) diffuse transmission (leaves/wax), (12) then
    GI.
  - Import-side note: every listed material extension is already
    parsed and preserved/logged by the Phase 4 importer (gate
    dispositions) — consumption here requires no importer rework;
    TEXCOORD_1/COLOR_0 vertex-layout growth rides the first
    sub-item that needs it.
  - **Showcase/benchmark scene (committed 2026-08-19): Amazon
    Lumberyard Bistro** (NVIDIA ORCA distribution, CC-BY 4.0) as the
    techniques-phase hero scene — its glass storefronts, emissive
    signage, many local lights, and alpha-masked foliage exercise the
    charter's transmission/clustered-Forward+/emissive/alpha features
    directly, and its exterior+interior scale carries the phase
    benchmarks. Requires a one-time curated FBX/USD→glTF conversion
    (no official glTF exists; conversion fidelity — alpha modes,
    normal-map orientation — is part of the task). Khronos Sponza
    remains Phase 4's fly-through scene (`--scene` takes any glTF, so
    no engine change is involved in the swap).
- **Scheduler sharing with host engines** (committed 2026-08-11, SDK
  phase): an embedding game engine must be able to make the renderer's
  task scheduler and its own job system ONE pool — via consumer-chosen
  worker budgets at creation (available from Phase 4 Stage 0) and, at the
  SDK surface, external-thread participation (host threads registered
  into the renderer's enkiTS scheduler). Rationale: the renderer must
  never starve host subsystems (audio, physics); idle workers sleep on
  semaphores and occupancy is bursty by design, but the end state is a
  single shared pool, not two polite ones.
- **Multi-language bindings** (committed 2026-08-10, SDK phase): the public
  API ships C-ABI-first — a single IDL source generates the C header, the
  C++ COM-lite header, and the DLL shim (bgfx precedent; kills header
  drift) — and each language binds the C header via its own native tool
  (Rust bindgen, Zig translate-c, C# P/Invoke generators, Python cffi,
  Lua FFI). SWIG is the recorded fallback if a broad scripting-language
  sweep is ever wanted directly from C++, but C-first is the strategy.
  The COM-lite surface discipline (PODs, no STL/exceptions, error codes)
  already satisfies every generator's input constraints by construction.
  **Paradigm-neutral consumer principle (2026-08-12):** consuming the
  engine requires NO particular paradigm of the caller — the binary ABI
  is a C-callable function-pointer table and the API is handle-based
  (create/set/destroy by handle), so procedural (C), ownership/trait
  (Rust), data-oriented (Zig), and OOP callers are all first-class. The
  C++ interface header is an ergonomic convenience, never a requirement;
  the engine imposes neither OOP nor an ECS on the host.
- **Main-loop ownership** (decided 2026-08-10, binds the SDK-phase spec):
  RendererX is library-model — the consuming game/engine owns `main()` and
  the frame loop and calls an explicit frame API (begin-frame → declare
  passes/submit → end-frame), never the reverse. No required init/frame
  callbacks, no engine-owned loop: the audience is custom-engine
  developers who own their own loops (bgfx/PhysX/FMOD precedent). An
  optional thin "runner" convenience (window + per-frame callback, e.g.
  over SDL3's callback mode) may ship later for quick starts, built ON the
  library API and never required by it.

- **Phase 4 exit-review registry items (2026-08-20):** (a) D27
  pre-resolution's "resolve once per distinct key" currently holds only
  for the opaque partition — blend-partition interleaving re-fires
  resolution per run (cost, not correctness); revisit with the
  techniques phase's transparency work. (b) Sample-recorder worker-side
  per-frame vector allocations (`splitByBlockAndGroup`/
  `resolveDrawGroups` return values) — extend the zero-alloc discipline
  to the sample/consumer recording path when the scene path is next
  reworked (geometry or techniques phase).

- **Layer-10 offline asset tooling — committed content inventory
  (2026-08-20):** everything the runtime currently regenerates per run
  is this phase's baking backlog, recorded here so none of it is lost:
  (a) texture baking — PNG/JPG → KTX2 (block-compressed + offline mip
  chains; `toktx` toolchain decision included), replacing the runtime
  stb decode+mip path for shipped content; (b) geometry baking —
  MikkTSpace tangent generation and meshoptimizer processing moved to
  import-time-once instead of every-run; (c) a **derived-data cache**
  (hash of source asset + processing parameters → cached processed
  blobs: decoded/mipped textures, tangents, optimized meshes) so even
  DEV-time imports of arbitrary content pay processing once per asset
  version, not per run; (d) the runtime stb/tangent paths REMAIN as
  the arbitrary-content fallback (dev convenience), never removed.
  Context: the per-run cost inventory and the pipeline-cache precedent
  (VkPipelineCache already persists per sample) are ledgered in the
  Phase 4 SDD (2026-08-20 entries).

- **Phase ordering ratified (owner, 2026-08-20):** Phase 5 =
  **Techniques** (the advanced material/lighting charter above, with
  minimal compute-pipeline capability pulled forward from the geometry
  phase as its Stage-0 prerequisite); Phase 6 = Geometry (meshlets,
  GPU-driven culling ladder, remaining compute scope); Phase 7 =
  Streaming/VFS (+ layer-10 offline asset tooling & derived-data
  cache, as two halves of one asset pipeline); Phase 8 = Animation;
  Phase 9 = SDK/DLL (ship line; tooling/profiling threads through
  phases 5-9 rather than standing alone). Plan documents remain the
  per-phase source of truth; this entry fixes only the sequence.
