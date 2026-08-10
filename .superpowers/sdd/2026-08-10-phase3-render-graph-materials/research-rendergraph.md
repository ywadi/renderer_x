# Phase 3 Render Graph — Prior Art Research

Scope: RendererX is Vulkan 1.3-only (dynamic rendering + synchronization2 baseline,
no `VkRenderPass`), bindless set 0, VMA, volk, Slang, custom RHI (`rx_rhi_vk`).
This document evaluates existing open render-graph designs/code as *port
material*, not as replacement RHIs. All claims below are sourced from GitHub's
REST API and raw file fetches performed in August 2026 unless marked
**UNVERIFIED**.

---

## 1. Granite (github.com/Themaister/Granite)

### License and activity
- License: **MIT**. Confirmed via GitHub API (`license.spdx_id: "MIT"`) and the
  repo's `LICENSE` file, copyright line `Copyright (c) 2017-2026 Hans-Kristian
  Arntzen`. — [LICENSE](https://github.com/Themaister/Granite/blob/master/LICENSE)
- Activity: **very actively maintained**. `pushed_at` = 2026-08-09 (i.e. the
  day before this research was run). Ten most recent commits span
  2026-07-27 → 2026-08-09, roughly one commit every 1-3 days (video decode,
  SPIR-V, ARM64 SIMD, DRM format-modifier fixes). 1,931 stars, 161 forks, only
  2 open issues. — [commit history](https://github.com/Themaister/Granite/commits/master)
- This is Hans-Kristian Arntzen's (Themaister, ex-RetroArch/Vulkan WG
  contributor, now at Valve on VKD3D-Proton) personal Vulkan renderer, in
  continuous development since January 2017.
  [github.com/Themaister/Granite](https://github.com/Themaister/Granite)

### Dynamic rendering vs VkRenderPass
**Still VkRenderPass/subpass-based — does NOT use `VK_KHR_dynamic_rendering`.**
Verified by direct source inspection (not documentation claims):
- `renderer/render_graph.cpp` calls `cmd.begin_render_pass(rp_info, ...)`,
  `cmd.next_subpass(...)`, `cmd.end_render_pass()` (lines 2246, 2266, 2270) and
  again at lines 2495/2497 for a secondary path.
  [render_graph.cpp](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)
- The physical-pass structure holds `Vulkan::RenderPassInfo render_pass_info`
  and `std::vector<Vulkan::RenderPassInfo::Subpass> subpasses` directly
  (`render_graph.hpp` lines 965-966).
  [render_graph.hpp](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.hpp)
- The backing implementation in `vulkan/render_pass.cpp` calls
  `vkCreateRenderPass`/`VkRenderPassCreateInfo` (5 occurrences) — classic
  renderpass objects, not `vkCmdBeginRendering`.
  [render_pass.cpp](https://github.com/Themaister/Granite/blob/master/vulkan/render_pass.cpp)
- Zero hits for `dynamic_rendering`, `BeginRenderingInfo`, or
  `vkCmdBeginRendering` anywhere in `render_graph.{hpp,cpp}` or
  `vulkan/command_buffer.cpp` (grepped directly).

### Synchronization2
**Yes — barrier derivation is sync2-native.** The graph's public resource-access
API and internal dependency bookkeeping are typed in sync2 terms throughout:
`VkPipelineStageFlags2`, `VkAccessFlags2` (66 hits in `render_graph.hpp`, 85 in
`render_graph.cpp`), and barrier emission uses `VkMemoryBarrier2` /
`VkBufferMemoryBarrier2` / `VkImageMemoryBarrier2` (`render_graph.cpp` lines
~1998, 2010, 2038, 2447). So: **barrier derivation already speaks sync2; only
the render-pass/subpass layer on top is legacy.** This is an important split
for the port (see §5).

### File sizes / structure
- `renderer/render_graph.hpp`: 31,412 bytes (1,094 lines).
- `renderer/render_graph.cpp`: 131,672 bytes (3,876 lines).
- Together this is by far the single largest subsystem in `renderer/`
  (`renderer.cpp` 48KB, `scene.cpp` 43KB, `ocean.cpp` 51KB, `mesh_util.cpp`
  34KB, all smaller) — the render graph is roughly 2.5-2.7× the size of the
  next-largest file in that directory.
  [renderer/ directory listing](https://github.com/Themaister/Granite/tree/master/renderer)

### Coupling to Vulkan::Device
**Tight at the call-surface level.** `render_graph.hpp`/`.cpp` directly
reference `Vulkan::Device`, `Vulkan::CommandBuffer`, `Vulkan::RenderPassInfo`,
`Vulkan::Semaphore`, `Vulkan::DeviceCreatedEvent`, etc. (47 references in the
header, 45 in the source — e.g. `RenderPassInterface::setup(Vulkan::Device&)`,
`RenderGraph::physical_pass_handle_signal(Vulkan::Device&, ...)`,
`EVENT_MANAGER_REGISTER_LATCH(RenderGraph, on_device_created,
on_device_destroyed, Vulkan::DeviceCreatedEvent)`). The graph is written
directly against Granite's own mid-level Vulkan wrapper, not an abstract
interface — there is no seam to swap in a foreign RHI without either building
a compatibility shim that mimics Granite's `Device`/`CommandBuffer` surface,
or extracting the algorithmic core and rewriting the execution glue.

### Which algorithms are cleanly portable
Verified against specific line ranges in `render_graph.{hpp,cpp}`:

| Algorithm | Location | Portability |
|---|---|---|
| Pass culling / dependency-graph construction | `traverse_dependencies`, `depend_passes_recursive` (`.hpp` 1030, 1033) | **Clean.** Pure graph traversal over declared reads/writes; touches no Vulkan types directly at this stage. |
| Resource lifetime analysis | `build_transients()`, `build_aliases()` (`.hpp` 980, 984), `physical_dimensions`/`physical_aliases` (`.hpp` 963, 1014) | **Clean in concept.** Assigns virtual resources to physical slots and lifetime ranges; needs remapping from `Vulkan::Image`/`RenderResource` to `rx_rhi_vk` handles. |
| Barrier derivation | `to_flush_access`/`invalidated_in_stage[64]` per-resource state (`.hpp` 996-1005), emission at `.cpp` ~1998-2038, 2447 | **Most directly reusable.** Already expressed as sync2 stage/access accounting; only the final "call the RHI's barrier submission function" needs re-pointing. |
| Transient attachment aliasing | `build_aliases()` body, "Find resources which can alias safely" (`.cpp` line 628) through claim logic (~line 790) | **Portable in concept, not in code.** Walks resource lifetimes and claims shared physical indices; currently entangled with `Vulkan::RenderPassInfo` attachment/subpass metadata that a dynamic-rendering RHI has no equivalent for — needs re-expression around a plain image-lifetime model. |
| Async compute scheduling | `get_queue_type` (`.cpp` 368-395), `physical_pass_handle_cpu_timeline`/`gpu_timeline` (`.cpp` 2324, 2380), `wait_for_semaphore_in_queue` (`.cpp` 1807) | **Portable at the scheduling-decision level** (which passes run on which queue, when cross-queue signal/wait is needed); execution glue against `Vulkan::Device`/`Vulkan::Semaphore` needs rewriting. |
| Render-pass/subpass merging & compatibility | `PhysicalPass::subpasses`, `Vulkan::RenderPassInfo::Subpass` (`.hpp` 965-966), `begin_render_pass`/`next_subpass`/`end_render_pass` sequencing (`.cpp` 2246-2270) | **Does not port.** This entire layer exists only because of `VkRenderPass`; under `VK_KHR_dynamic_rendering` there is no subpass merging or render-pass-compatibility caching to replicate — it is deleted outright and replaced by a per-pass `vkCmdBeginRendering`/`vkCmdEndRendering` built from the attachment list the graph already derives. |

### Hans-Kristian Arntzen's render-graph blog posts
- **"Render graphs and Vulkan — a deep dive"** (2017-08-15) — the foundational
  and, as far as this research could determine, still the *only* dedicated
  render-graph design post. Explicitly credits Yuriy O'Donnell's GDC 2017
  Frostbite FrameGraph talk as inspiration, and covers barriers, subpass
  optimization, and async compute for a `VkRenderPass`-era Vulkan.
  [themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)
- **"A tour of Granite's Vulkan backend" series** (2019-04, Parts 1, 2, 5
  confirmed reachable) — covers the mid-level `Vulkan::Device`/`CommandBuffer`
  abstraction the graph sits on (object lifetimes, memory management), not
  the graph design itself.
  [Part 1](https://themaister.net/blog/2019/04/14/a-tour-of-granites-vulkan-backend-part-1/),
  [Part 2](https://themaister.net/blog/2019/04/17/a-tour-of-granites-vulkan-backend-part-2/),
  [Part 5](https://themaister.net/blog/2019/04/27/a-tour-of-granites-vulkan-backend-part-5/)
- **"Walking backwards into the future – a look at descriptor heap in
  Granite"** (2026-03-29) — about the descriptor-binding model
  (`VK_EXT_descriptor_heap` / bindless migration), not the render graph.
  [themaister.net/blog/2026/03/29/...](https://themaister.net/blog/2026/03/29/walking-backwards-into-the-future-a-look-at-descriptor-heap-in-granite/)
- **No dedicated "render graph redesign" or "migrating to dynamic rendering"
  post was found**, despite Granite itself being under continuous active
  development through August 2026 and the graph already having been updated
  internally to sync2 types. I checked the `tag/granite` and
  `category/vulkan` archive pages on themaister.net directly and found no
  such post in either. This absence is corroborated by direct source
  inspection (the graph is still renderpass-based today) but the *absence of
  a blog post* is inherently a negative claim — treat as well-supported but
  **not exhaustively provable** (Arntzen could have written about it on a
  platform outside his own blog that this research did not check, e.g. a
  conference talk or forum post).

---

## 2. skaarj1989/FrameGraph

### License and maintenance
- License: **MIT**. Confirmed via GitHub API (`license.spdx_id: "MIT"`).
  [LICENSE](https://github.com/skaarj1989/FrameGraph/blob/master/LICENSE)
- Created 2022-02-09, `pushed_at` 2025-12-03. Not archived. 276 stars, 3 open
  issues.
  [github.com/skaarj1989/FrameGraph](https://github.com/skaarj1989/FrameGraph)
- **Maintenance level: low / essentially dormant with occasional drive-by
  patches.** Recent commit log:
  `f84d671a 2025-12-03 "Add a snippet for addCallbackPass"`,
  `add129ac 2025-12-03 "Fix tests"`,
  `c1cdaf35 2024-01-01 "Update LICENSE"`,
  `29d0ed5a 2023-11-24 "Add visualization tool"`,
  `d76aeddb 2023-11-17 "Refactoring"` — substantive work stopped roughly
  November 2023; the two 2025-12-03 commits are a doc snippet and a test fix,
  not feature work.
  [commit history](https://github.com/skaarj1989/FrameGraph/commits/master)

### API shape (verified by reading `include/fg/FrameGraph.hpp`, `TypeTraits.hpp`, `ResourceEntry.hpp`)
- **Virtual resources**: `FrameGraphResource` handles, typed via a
  `T::Desc` template parameter (any concrete resource wrapper `T` the app
  supplies).
- **Builder / setup+execute lambda pattern**: `FrameGraph::addCallbackPass<Data>(name, setup, exec)` —
  `setup` runs immediately and declares `create`/`read`/`write` on a
  `Builder`; `exec` is deferred until `FrameGraph::execute()` and must
  capture by value. `Builder::setSideEffect()` exempts a pass (e.g. present)
  from culling.
- **`compile()`** performs pass/resource culling of anything unreferenced
  (this is the "pass culling" step, analogous to Granite's but far simpler —
  no queue/aliasing/barrier concerns baked in).
- **Blackboard**: a separate `Blackboard.hpp`/`.inl` class exists specifically
  for cross-pass/cross-module data sharing (the GDC-talk "blackboard"
  pattern), decoupled from `FrameGraph` itself.
- Header sizes total roughly 25-30KB across all of `include/fg/*.hpp` — small
  relative to Granite's 163KB `render_graph.{hpp,cpp}`, which reflects how
  much less this library actually does.
  [include/fg directory](https://github.com/skaarj1989/FrameGraph/tree/master/include/fg)

### What it deliberately does NOT do
**Confirmed by reading source directly**: `TypeTraits.hpp` defines
`has_preRead`/`has_preWrite` concepts (lines 24-29), and `ResourceEntry.hpp`
calls the resource's optional `preRead(Desc, flags, context)` /
`preWrite(Desc, flags, context)` hooks if the concrete resource type
implements them (lines 56-81). **That is the entire extent of its
synchronization support** — it does not derive barriers, does not compute
layout transitions, and does not allocate any GPU resource itself; 100% of
that logic is left to whatever `T` (the app's own Vulkan resource wrapper)
chooses to do inside those two optional callbacks.
[TypeTraits.hpp](https://github.com/skaarj1989/FrameGraph/blob/master/include/fg/TypeTraits.hpp),
[ResourceEntry.hpp](https://github.com/skaarj1989/FrameGraph/blob/master/include/fg/ResourceEntry.hpp)

### Realistic integration cost on rx_rhi_vk
Because it does zero synchronization work, "integrating" this library does
not remove the hard part of the problem — you still need a Granite-class
barrier/aliasing/queue-scheduling layer underneath it; FrameGraph only
supplies the DAG + culling + blackboard scaffolding around that layer. Given
the current maintenance level (essentially frozen since Nov 2023, only
doc/test drive-bys since), treat it as a **design reference / skeleton**
rather than a dependency to build production code on top of as-is.
A production integration would mean: (1) vendoring the ~1,500-line core
verbatim or near-verbatim (small enough that "vendor and maintain ourselves"
is a realistic option under the project's own "port, don't reinvent" rule),
then (2) writing the entire barrier/layout/transient/queue layer from scratch
or by porting it from Granite — i.e., FrameGraph does not reduce the amount
of sync2/dynamic-rendering-specific work RendererX has to write; it only
gives a proven shape for the DAG/culling/blackboard part.

---

## 3. AMD Render Pipeline Shaders (RPS) SDK

### License
**MIT — but this was not always true.** The *current* `LICENSE.txt` on `main`
is plain MIT (`Copyright (c) 2024 Advanced Micro Devices, Inc.`), confirmed
by fetching the raw file.
[LICENSE.txt](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/blob/main/LICENSE.txt)
However, the **very first commit** (`ce66a655`, 2022-12-13, "AMD Render
Pipeline Shaders SDK Open Beta") shipped a `LICENSE.rtf` whose text literally
begins `INTERNAL EVALUATION LICENSE` — confirmed by fetching that historical
blob and grepping its contents. Three days later
(`3dbf9b17`/`2c304e58`, 2022-12-16, "Plaintext version of the license" /
"Update README to point to plaintext license") it was replaced with the MIT
`LICENSE.txt` that remains current today. **Net: the SDK you would actually
depend on today is MIT-licensed**, but its origin as an AMD internal-eval
drop is worth knowing if provenance is ever questioned.
[initial commit tree](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/commit/ce66a655)

### Vulkan backend maturity vs D3D12
Public docs/README make no explicit maturity comparison between backends
(searched; found none) — mark the *qualitative* "which is more mature"
question **UNVERIFIED against official statements**. However, direct source
inspection of `src/runtime/vk/rps_vk_runtime_backend.cpp` (1,934 lines) shows
the Vulkan backend is a complete, feature-parallel implementation that:
- creates and begins classic render passes: `vkCreateRenderPass` (line 1313),
  `vkCmdBeginRenderPass` (line 720), `CreateRenderPasses(...)` (line 1107) —
  **legacy VkRenderPass, not `VK_KHR_dynamic_rendering`**;
- issues barriers via legacy `vkCmdPipelineBarrier` (line 1608) —
  **not synchronization2** (`VkPipelineBarrier2`/`vkCmdPipelineBarrier2` do
  not appear).
[rps_vk_runtime_backend.cpp](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/blob/main/src/runtime/vk/rps_vk_runtime_backend.cpp)
So regardless of any stated maturity gap, **both of RendererX's baseline
requirements (dynamic rendering, sync2) are absent from RPS's Vulkan backend
today** — this is the load-bearing finding for RendererX's purposes, and it's
verified by reading the actual backend code rather than by a maturity claim.
There is one open GitHub issue asking "when will have vulkan example on
linux?" (#34, still open) which is weak secondary evidence of the Vulkan path
getting less attention than D3D12 in practice.
[issue #34](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/issues/34)

### Integration model — how much control the runtime takes
RPS is a **compiler-style, high-control-taking** system, qualitatively
different from Granite or FrameGraph: applications author render-graph logic
in **RPSL**, a custom HLSL-like DSL compiled by RPS's own `rps_hlslc` tool
into a "linear node sequence"; the RPS runtime then compiles that sequence
into a graph, schedules it, and its backend (the file inspected above) is
the thing that actually calls `vkCreateRenderPass`, `vkCmdBeginRenderPass`,
and `vkCmdPipelineBarrier` on the application's behalf. The application
supplies callback functions for the actual draw/dispatch work but does not
own barrier insertion, transient-memory aliasing, or (per the backend code)
render-pass object creation — RPS's own framing is that it aims to be "a
generally optimal barrier generator and (aliasing) memory scheduler" that
the app plugs shader/draw callbacks into, rather than a library the app
drives step-by-step.
[GPUOpen RPS overview](https://gpuopen.com/rps/),
[Introducing RPS SDK](https://gpuopen.com/learn/rps_1_0/)

### Maintenance status
**Effectively stalled.** Releases: `open_beta_1.1.1_maintenance` (2024-05-14),
`open_beta_1.1_maintenance` (2023-08-11), `open-beta-1.1` (2023-05-03),
`open-beta` (2022-12-15) — four releases in under two years, then nothing.
`pushed_at` = 2024-05-17 (latest commit), i.e. **over two years with no code
push** as of this research (Aug 2026), and the project is still labeled
"Open Beta" roughly four years after its initial 2022 release. 16 open
issues, not archived.
[releases](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/releases),
[repo](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders)

---

## 4. Other open C++ render-graph land, 2024-2026 (excluding whole-RHI frameworks)

Searched broadly for anything new or newly active; excluding vuk, Diligent,
bgfx, The Forge, and any project that is really "adopt our whole RHI."

- **DragonJoker/RenderGraph** — [github.com/DragonJoker/RenderGraph](https://github.com/DragonJoker/RenderGraph).
  MIT license. Created 2019, but genuinely **actively maintained through
  2026**: `pushed_at` 2026-05-10, recent commits include
  "Fixed an issue when source and target buffer are the same during a copy"
  (2026-05-10), "Reworked callbacks interface" (2026-05-04). 250 stars, only 2
  open issues.
  [commit history](https://github.com/DragonJoker/RenderGraph/commits/master)
  - It is a genuinely **standalone** library (built by the author of the
    Ashes RHI but not dependent on it) — its `vcpkg.json` lists only
    `vulkan-headers` as a runtime dependency, and its `.gitmodules` pulls in
    only a generic CMake-utils submodule and vcpkg itself, no Ashes code.
    [vcpkg.json](https://raw.githubusercontent.com/DragonJoker/RenderGraph/master/vcpkg.json)
  - API: you register `FramePass`es with typed attachments (input/sampled/
    color/depth-stencil/etc.), and it builds a `RunnableGraph` that derives
    image-layout transitions automatically during execution
    (`GraphContext`/`RunnablePass`/`ResourceHandler` in `include/RenderGraph/`).
  - **However, it does not meet RendererX's baseline**: direct source
    inspection of `include/RenderGraph/GraphContext.hpp` shows
    `DECL_vkFunction(CreateRenderPass)`, `DECL_vkFunction(CmdBeginRenderPass)`,
    `DECL_vkFunction(CmdPipelineBarrier)` — **classic `VkRenderPass` and
    classic (non-sync2) barriers**, no `dynamic_rendering`, no
    `PipelineBarrier2`/`*Barrier2` structs anywhere in that header.
    [GraphContext.hpp](https://github.com/DragonJoker/RenderGraph/blob/master/include/RenderGraph/GraphContext.hpp)
  - Net: this is the most credible "new-ish, standalone, still-alive"
    render-graph library found outside Granite/FrameGraph/RPS, but it would
    need the *same* renderpass→dynamic-rendering and barrier→sync2 rewrite
    that Granite's graph would, while being a less battle-tested, less
    documented, less blogged-about codebase than Granite. It does not change
    the ranking in §5.

- No other actively-maintained, dynamic-rendering-native, sync2-native,
  standalone (non-whole-RHI) C++ render-graph library surfaced in this
  search. Community-written *articles* about render-graph design continue to
  appear (e.g. Riccardo Loggini's 2021 "Render Graphs" post, Traverse
  Research's "Render Graph 101", a 2025-04-21 "Building a Vulkan Render
  Graph" post by Tony Adriansen) but these are blog-post walkthroughs, not
  maintained libraries, and every one surveyed explicitly cites Granite's
  2017 design as its reference point rather than superseding it. Treat the
  claim "nothing meaningfully new displaced Granite as the reference design
  2024-2026" as well-supported by the search but not provable as a negative;
  mark as **UNVERIFIED (absence claim)**.
  [Render Graphs – Riccardo Loggini](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html),
  [Render Graph 101 – Traverse Research](https://blog.traverseresearch.nl/render-graph-101-f42646255636),
  [Building a Vulkan Render Graph – Tony Adriansen](https://tadriansen.dev/2025-04-21-building-a-vulkan-render-graph/)

---

## 5. Recommendation inputs

**Key fact that reframes the whole comparison**: none of Granite, RPS SDK, or
DragonJoker/RenderGraph use `VK_KHR_dynamic_rendering` today — all three are
built around classic `VkRenderPass`/subpasses (verified by source inspection
in §§1, 3, 4). Only Granite's barrier layer is already sync2-native; RPS and
DragonJoker/RenderGraph use legacy `vkCmdPipelineBarrier`. skaarj1989/FrameGraph
is barrier-API-agnostic by design (it does none of that work itself), so it
doesn't carry this liability but also doesn't carry any of the benefit.

This means **every option below requires the same fundamental rewrite**: the
render-pass/subpass compatibility-and-merging layer must be deleted and
replaced with per-pass `vkCmdBeginRendering`/`vkCmdEndRendering`, and any
legacy-barrier-API code must be re-pointed at sync2 (`vkCmdPipelineBarrier2`
and `*Barrier2` structs). What differs between the options is how much of the
*rest* (pass culling, resource lifetime/aliasing, queue scheduling) you get
for free and how directly it maps onto sync2 concepts already.

### (a) Pure Granite design/algorithm port onto rx_rhi_vk
**Rank: 1 (recommended).**
- Pros: barrier-derivation state machine (invalidate/flush accounting in
  sync2 terms) ports almost directly — it's already speaking the same
  `VkPipelineStageFlags2`/`VkAccessFlags2` vocabulary RendererX's RHI uses;
  pass-culling and dependency-graph-traversal algorithms are pure graph code
  with no Vulkan coupling to strip; transient/aliasing logic, while
  currently entangled with `Vulkan::RenderPassInfo`, is a well-understood,
  battle-tested algorithm (in production in Granite since ~2017, MIT
  licensed, actively maintained through Aug 2026) that is worth re-deriving
  around a plain lifetime model.
- Risks: (1) the only part of the port that is genuinely new work rather
  than translation is deleting the subpass/merging layer and replacing it
  with dynamic-rendering attachment setup — this is a real design task, not
  a mechanical port, because Granite's subpass-merging heuristics (which
  passes can share a renderpass) have no equivalent under dynamic rendering
  and must be replaced by a decision about whether to still batch adjacent
  passes for other reasons (e.g. avoiding redundant image-layout churn) or
  simply drop batching entirely; (2) tight coupling to `Vulkan::Device`/
  `Vulkan::CommandBuffer` means this is a "read Granite's ~5,000 lines
  closely and re-implement the algorithm against `rx_rhi_vk` types," not a
  drop-in vendor — budget it as a from-scratch implementation *guided by* a
  proven reference, not a code import; (3) MIT license is compatible with
  a from-scratch reimplementation-by-reading in any case, so there is no
  legal blocker either way.

### (b) Hybrid — skaarj1989/FrameGraph skeleton + Granite-derived barrier/aliasing logic in the execute layer
**Rank: 2.**
- Pros: FrameGraph's DAG/culling/blackboard scaffolding (`addCallbackPass`,
  `Builder::create/read/write`, `compile()`, `Blackboard`) is small
  (~1,500 lines), simple, and MIT-licensed, so vendoring it (or reimplementing
  its shape) is cheap; because it does zero synchronization work itself
  (confirmed via `preRead`/`preWrite` hook inspection in §2), there's no
  legacy-Vulkan-API code to rip out — you write the sync2/dynamic-rendering
  logic once, cleanly, in the `preRead`/`preWrite`-equivalent hooks you
  design for `rx_rhi_vk` resources.
- Risks: (1) FrameGraph is essentially unmaintained since Nov 2023 (§2) — you
  are adopting a frozen skeleton, not a supported dependency, so treat any
  bugs found as yours to fix permanently; (2) it does not include Granite's
  resource-lifetime/aliasing or async-compute-queue-scheduling algorithms at
  all — those still have to come from a Granite-style port for the
  hybrid to reach production quality, meaning this option's "cost savings"
  over (a) are limited to the DAG/culling/blackboard plumbing, which is the
  smallest and least risky part of a render graph to write from scratch
  anyway; (3) gluing two independently-designed pieces (FrameGraph's
  resource-handle/version model plus Granite's barrier-state-machine
  concepts) risks impedance mismatches that a from-scratch design tailored
  to `rx_rhi_vk` would not have.

### (c) DragonJoker/RenderGraph as an alternative base
**Rank: 3 (not recommended as primary path, worth a closer look only if (a) stalls).**
- Pros: standalone (only depends on vulkan-headers, confirmed via
  `vcpkg.json`/`.gitmodules`), genuinely actively maintained through 2026
  (unlike FrameGraph or RPS), does derive layout transitions automatically
  (more than FrameGraph gives you), MIT licensed.
- Risks: (1) same legacy-VkRenderPass and legacy-barrier problem as Granite
  and RPS (confirmed via `GraphContext.hpp` inspection, §4) — so it buys
  nothing over Granite on the "how much needs rewriting" axis while being
  far less documented, less blogged-about, and less proven at scale than
  Granite (250 stars / one maintainer vs. Granite's 1,931 stars, years of
  blog-documented design rationale, and its role as the actual render graph
  shipping in Themaister's production work); (2) no public design writeup
  exists to shortcut understanding the way Arntzen's 2017 post does for
  Granite — the "port cost" is dominated by reading undocumented source.

### Does Granite being renderpass-centric change the ranking?
**No — and this is the central finding of this research.** All three
concrete implementations surveyed (Granite, RPS, DragonJoker/RenderGraph) are
renderpass-centric; only the API-agnostic FrameGraph avoids the problem by
doing no synchronization work at all. Since dynamic-rendering support has to
be designed fresh regardless of which base is chosen, the deciding factor
becomes *which parts of the port are mechanical translation vs. fresh
design*, and Granite wins that comparison specifically because its barrier
derivation is **already sync2-native** (§1) — that is the single largest,
highest-risk piece of a render graph to get right, and it is the one piece
Granite has already solved in the exact vocabulary RendererX's RHI uses. The
part that must change — subpass/transient-attachment merging logic — is
comparatively small, self-contained, and well-understood (it reduces to "for
each pass, gather its attachments and issue one `vkCmdBeginRendering`/
`vkCmdEndRendering` pair," a strict simplification relative to Granite's
current subpass-compatibility bookkeeping, not an expansion of scope).
Ranking stands: **(a) pure Granite algorithm port > (b) FrameGraph hybrid >
(c) DragonJoker/RenderGraph**, with (b) worth keeping in reserve as a cheap
DAG/culling skeleton if the team wants to avoid writing that (smallest) part
from scratch, and (c) worth a second look only if Granite's source turns out
harder to extract algorithms from in practice than this research estimates.
