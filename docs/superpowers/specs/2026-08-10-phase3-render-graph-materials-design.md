# Phase 3: Render Graph + Material System — Design

**Date:** 2026-08-10
**Status:** Approved for implementation
**Research inputs:**
- `.superpowers/sdd/2026-08-10-phase3-render-graph-materials/research-rendergraph.md` — cited as [R:G§n]
- `.superpowers/sdd/2026-08-10-phase3-render-graph-materials/research-abi-materials.md` — cited as [R:M§n]

## Goal

Add layers 6-7 of the renderer: a dynamic-rendering-native render graph
(`rx_graph`) that derives execution order, synchronization2 barriers, image
layout transitions, and transient resource management from declarative pass
descriptions; and a material system (`rx_material`) exposing the project's
first engine-facing public API surface (`IMaterial` et al.) in an
ABI-stable shape, built on runtime Slang link-time specialization. The
phase exits with two deployed samples (05_multipass: shadow + forward +
tonemap; 06_materials: material authoring, per-instance overrides, hot
reload), green CI on both platforms, and release v0.3.0-phase3.

## Fixed decisions

### D1 — Render graph is a from-scratch implementation of Granite's algorithms, not a vendored dependency

Granite's render graph (Themaister/Granite, MIT, actively maintained
through Aug 2026) is the reference: its barrier derivation is already
synchronization2-native (per-resource invalidate/flush accounting in
`VkPipelineStageFlags2`/`VkAccessFlags2` terms), which is the exact
vocabulary rx_rhi_vk speaks [R:G§1]. But the graph is ~5,000 lines tightly
coupled to Granite's own `Vulkan::Device`/`CommandBuffer` wrapper — there
is no seam to swap in a foreign RHI [R:G§1]. Therefore: **re-implement the
algorithms against rx_rhi_vk types, guided by Granite's source and
Arntzen's design writeups** (the 2017 "Render graphs and Vulkan — a deep
dive" post remains the canonical design document [R:G§1]). Budget it as a
from-scratch implementation guided by a proven reference, not a code
import [R:G§5].

**Rejected alternatives** (recorded per the repo's "don't reinvent the
wheel / say so explicitly" rule):

- **AMD Render Pipeline Shaders SDK** — MIT, but effectively stalled (no
  commits since May 2024, still "Open Beta"); its Vulkan backend uses
  legacy `VkRenderPass` + legacy `vkCmdPipelineBarrier` (neither of our
  baselines); and its compiler-style runtime owns barrier insertion and
  render-pass creation on the app's behalf — adopting it inverts control
  of the renderer [R:G§3].
- **skaarj1989/FrameGraph** — MIT but dormant since Nov 2023; by design
  does zero barrier/layout work (only optional `preRead`/`preWrite`
  hooks), so it removes none of the hard work; kept as a design reference
  for the DAG/culling/blackboard API shape only [R:G§2, §5].
- **DragonJoker/RenderGraph** — standalone and actively maintained, but
  legacy `VkRenderPass` + legacy barriers like the others, with no public
  design writeup; buys nothing over Granite while being less proven
  [R:G§4].
- **vuk (or any whole-RHI framework: Diligent, bgfx, The Forge)** — out of
  scope by construction: RendererX owns its RHI. That was decided in
  Phase 1 for the same reason it holds now: the renderer's public surface,
  resource model, and extension points are the product; the genuinely
  reusable subsystems inside the RHI (device bootstrap, allocation,
  loading, windowing, shader compilation) are all already ready-made
  libraries (vk-bootstrap, VMA, volk, SDL3, Slang). Adopting a framework
  would make RendererX a wrapper around someone else's opinionated API and
  forfeit the Vulkan-1.3-only payoff (direct dynamic rendering, sync2,
  bindless) to multi-backend abstraction. This paragraph is the explicit
  "written from scratch, and here's why" record the repo policy requires.

Key research finding that de-risks D1: **every** surveyed implementation
is renderpass-centric, so dynamic-rendering support is fresh design work
no matter which base is chosen; only Granite arrives with the sync2
barrier machinery — the largest, highest-risk piece — already solved
[R:G§5].

### D2 — The graph is dynamic-rendering-native; Granite's subpass layer is deleted, not ported

Granite's render-pass/subpass compatibility-and-merging layer exists only
because of `VkRenderPass`; under dynamic rendering it has no equivalent
and is replaced by a per-pass `vkCmdBeginRendering`/`vkCmdEndRendering`
pair built from the attachment list the graph already derives [R:G§1
table, §5]. This is a scope reduction. No pass-batching heuristics in
Phase 3: one `vkCmdBeginRendering` scope per graphics pass. (Batching
adjacent passes to reduce layout churn is a possible later optimization;
the barrier model already minimizes redundant transitions.)

### D3 — Barrier derivation ports Granite's sync2 invalidate/flush accounting model

Per physical resource, track flushed writes (`VkPipelineStageFlags2` +
`VkAccessFlags2` of the last write) and per-stage invalidations (which
stages/access have already seen a matching cache invalidate), plus current
`VkImageLayout`. Walking passes in submission order, emit
`VkImageMemoryBarrier2`/`VkBufferMemoryBarrier2`/`VkMemoryBarrier2`
exactly when a pass's declared access requires a flush, an invalidate, or
a layout change not already satisfied [R:G§1]. Execution uses the existing
sync2 path (`vkCmdPipelineBarrier2` via volk). Write-after-read and
read-after-read chains must not emit redundant barriers (asserted by unit
tests on exact barrier sequences).

### D4 — Phase 3 graph feature set (and explicit deferrals)

In scope: graphics and compute passes; declared attachment writes
(color/depth), attachment/texture reads, buffer reads/writes; pass culling
from declared outputs (side-effect flag exempts presentation); automatic
barriers + layout transitions (D3); transient attachment pooling;
swapchain integration with the existing `FrameSync` (frames-in-flight=2)
and per-image semaphores; per-pass GPU debug labels
(`vkCmdBeginDebugUtilsLabelEXT` when available).

Explicit deferrals (deferred, not dropped; recorded here as the scoping
call):

- **Async compute scheduling.** The pass API carries a queue-class hint
  (`Graphics`/`AsyncCompute`) so declarations are future-proof, but the
  Phase 3 scheduler maps every pass to the graphics queue. Cross-queue
  ownership transfer and semaphore scheduling multiply barrier complexity
  and testing burden, and the Phase 3 samples cannot demonstrate a win.
  Granite's queue-scheduling decisions are portable later [R:G§1 table].
- **Intra-frame transient memory aliasing.** Lifetime analysis (first/last
  use per physical resource) is computed and tested in Phase 3, but
  physical backing is a transient pool with cross-frame reuse keyed by
  descriptor (format/extent/usage/samples), not placed-aliased memory.
  Granite's aliasing walk is entangled with renderpass attachment metadata
  and needs re-derivation around a plain lifetime model [R:G§1 table];
  the memory win is irrelevant at sample scale. The lifetime data this
  phase produces is the input the aliasing allocator needs later.

### D5 — Public API surface uses COM-lite pure-virtual interfaces; the standalone DLL artifact is deferred

The DLL is zig/MinGW-built (`*-windows-gnu`) and consumers are MSVC-built.
Research verdict [R:M§1.2]: Windows x64 has one OS-mandated calling
convention (no x86-style `thiscall` divergence), and the COM-legal vtable
subset — single inheritance from a pure-virtual root, no data members, no
overloads, no virtual destructor — is empirically interoperable between
MinGW-family and MSVC compilers, proven by mingw-w64's decades of COM
consumption and by Slang's own `ISlangUnknown`, which RendererX already
calls through today. Exported real C++ classes are unsafe across this
pairing (name mangling, exception ABI, RTTI, STL layout, CRT heaps)
[R:M§1.1c]. A flat C ABI is maximally safe but forfeits C++ ergonomics
for a C++-first engine surface [R:M§1.5].

Decision: **`IRxUnknown`-rooted COM-lite interfaces** modeled on Slang's
in-repo precedent — `queryInterface(GUID, void**)` / `addRef()` /
`release()`, GUID per interface version, `extern "C"` factory entry
points, error codes (`RxResult`) never exceptions, no STL or RTTI across
the boundary, allocation and deallocation on the renderer side only, POD
boundary structs with explicit static-asserted layout [R:M§1.3, §1.5].

The **standalone DLL artifact is deferred** (the user already relaxed the
single-DLL requirement in Phase 1). Phase 3 implements the interfaces in
the ABI-safe shape inside the normal static-library build and proves them
by making sample 06 consume materials exclusively through the public
surface. Shipping `rx.dll` later is then packaging, not an API break.

### D6 — Material model: material = Slang module implementing an engine-defined Slang interface

Follows the Shader Components lineage (He et al. 2017) and its production
evolution in Slang, with Falcor as the shipping precedent for
Slang-interface-based materials [R:M§2.2].

- Shader side: `shaders/material/material.slang` defines
  `interface IMaterialShader` (surface evaluation entry, with default
  implementations where sensible so materials override only what differs
  — the inherit/override/extend requirement maps to Slang interface
  defaults + module composition [R:M§2.3]). A material is a `.slang`
  module declaring a struct that conforms to `IMaterialShader` plus a
  `ParameterBlock<TParams>` for its bound parameters
  (`ParameterBlock<T>` is Slang's native "one buffer + one descriptor
  set" primitive [R:M§2.2]).
- Host side: `rx::material::Material` loads the module through the
  existing `rx_shader::Compiler`, hashes module content, and reflects
  `TParams` through the existing `reflect()` path.
- Specialization: static link-time specialization via
  `createCompositeComponentType` + `link` per unique (material,
  pass-signature) pair — Slang's documented recommended workflow
  [R:M§2.2]. Existential/`anyValueSize` dynamic dispatch is **not** used
  in Phase 3 (reserved for a future bindless-material-array case
  [R:M§3.3]).

### D7 — Pipeline variants: lazy, content-hash-keyed cache (Granite/Fossilize model, not Filament exhaustive precompilation)

Cache key = (material module content hash, pass-signature hash,
specialization bitmask). Pass signature is derived from the render graph
pass declaration: color attachment formats, depth format, sample count,
and pipeline-relevant fixed state — the graph is the source of per-pass
variability, so the key is generated, not hand-enumerated [R:M§3.3].
Variants compile lazily on first (material, pass) use; compiled
`VkPipeline`s live in a `VkPipelineCache` that is loaded from / saved to
disk across runs (cheap, standard). Fossilize integration and
Filament-style offline exhaustive variant packaging are deferred
[R:M§3.1-3.3].

### D8 — Parameters vs. specialization split at the API level

Mirroring Filament's parameter/constant split [R:M§2.1]: **bound
parameters** (the `ParameterBlock` instance data + texture bindings via
the existing `BindlessTable`) change per-instance and per-frame with zero
recompilation; **specialization inputs** (module choice, specialization
bitmask) are set at material/instance creation and route through the
variant cache. `IMaterialInstance` exposes only typed parameter setters
and texture assignment; nothing on the instance can trigger a shader
recompile mid-frame.

### D9 — Hot reload invalidates by module content hash

A reload event re-hashes the module; a changed hash invalidates exactly
the (material, pass) cache entries derived from that module, re-links,
and rebuilds those pipelines — keep-last-good on compile failure, exactly
as sample 02 established [R:M§2.3]. Fresh `Compiler` per reload (the
documented rx_shader same-module-name caveat). Pipeline destruction goes
through the existing fence-gated `DeletionQueue`.

### D10 — Sample 05_multipass: shadow + forward + tonemap through the graph

Directional-light shadow map (depth-only pass, transient D32 target) →
forward lit pass sampling the shadow map (bindless) → tonemap
post-process (fullscreen pass reading the HDR color transient, writing
the backbuffer). All passes, resources, barriers, and transitions
declared/derived through rx_graph; zero hand-written barriers in the
sample. Headless gate: render N frames, read back the final image,
assert lit/shadowed/tonemapped pixel expectations; `--present` mode with
animated light. This is the acceptance test for D1-D4.

### D11 — Sample 06_materials: the public surface, exercised end to end

Multiple objects, at least two distinct material modules, per-instance
parameter overrides — consumed **exclusively through the COM-lite public
interfaces** (factory → `IMaterial` → `IMaterialInstance`), proving D5-D8.
Present mode watches material files and hot-reloads (D9). Headless gate:
compile materials, render, read back, assert per-instance override
pixels differ as specified; plus an interface-contract test
(queryInterface identity, refcount round-trip, error codes on bad input).

### D12 — Testing bar

Unchanged from Phases 1-2: zero validation errors (gates run `--validate`),
every graph/material subsystem unit-tested (exact barrier sequences,
culling, lifetime ranges, cache keying, refcount/QI contract), headless
pixel gates unconditional, both presets green in CI, packaged samples
remain unzip-and-run.

## Architecture

```
samples/05_multipass, 06_materials
        │
src/rx_material      — material model, variant cache, public COM-lite surface
        │  (pass signatures)
src/rx_graph         — declarative passes, culling, barriers, transients, execution
        │
src/rx_shader        — Slang compile/reflect (existing)
src/rx_rhi_vk        — device, swapchain, bindless, upload, sync (existing)
```

- `rx_graph` depends on `rx_rhi_vk` only. Public headers:
  `src/rx_graph/include/rx_graph/` (`render_graph.h`, `pass.h`,
  `resources.h`, `barriers.h` internal-detail split per implementation).
- `rx_material` depends on `rx_graph` + `rx_shader` + `rx_rhi_vk`.
  Public COM-lite headers: `src/rx_material/include/rx_material/rx_api.h`
  (the ABI surface: `IRxUnknown`, `IMaterial`, `IMaterialInstance`,
  `RxResult`, GUIDs, `extern "C"` factories) plus internal C++ headers.
- Shader-side material interface lives in `shaders/material/`.

## Exit criteria

1. All unit tests + headless gates green on linux-native and
   windows-cross-zig CI; zero validation errors under `--validate`.
2. Samples 05 and 06 in the per-platform packages, unzip-and-run.
3. Ledger complete; final whole-branch review clean after at most one fix
   wave.
4. Tag + release **v0.3.0-phase3** with CI-built packages.

## Deferred (not dropped) — Phase 3 additions to the standing list

Async compute execution (D4); intra-frame transient aliasing (D4);
standalone DLL packaging (D5); existential dynamic dispatch for bindless
material arrays (D6); Fossilize/offline variant tooling (D7); pass
batching for layout-churn reduction (D2).
