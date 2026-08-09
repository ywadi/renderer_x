# RendererX Phase 2: Slang Runtime Shader System + Resource Management — Design

Status: Approved (coordinator-authored under standing full-auto mandate)
Date: 2026-08-10
Research basis: `.superpowers/sdd/2026-08-10-phase1-completion/phase2-research-findings.md`
(all version-specific claims below are verified there with citations; section refs like
[R:A2] point into that file)

## Scope

Phase 2 delivers layers 4 and 5 of the renderer:

- **Layer 4 — Shader system:** in-process Slang compilation (source → SPIR-V) with
  diagnostics, hot reload, and reflection that drives `VkDescriptorSetLayout` /
  `VkPipelineLayout` creation automatically.
- **Layer 5 — Resource management:** staging/upload (ReBAR-aware with fallback),
  device-local textures with generated mips, mesh buffers, a global bindless descriptor
  table, and fence-keyed deferred destruction.
- **Exit criterion:** three runnable samples (hot reload, bindless meshes, streaming)
  built for Linux + Windows, deployed via CI artifacts and a tagged GitHub release.

Out of scope (deferred, not dropped): render graph (layer 6), materials/IMaterial (layer 7),
KTX2/BasisU compressed textures (goes with the asset-import layer — [R:C3]), compute-based
mip generation (blit chain first — [R:C2]), dedicated transfer-queue uploads (single-queue
path first, transfer queue as later optimization — [R:C1]), Slang's native bindless sugar
(`bindless-storage.slang`, `-bindless-space-index`) — promising but brand-new in the pinned
version; hand-rolled global set is the Phase 2 baseline, native sugar is a follow-up spike
[R:B3, R:E2].

## Fixed decisions (from verified research — do not relitigate in implementation)

1. **Slang linkage:** `find_package(slang CONFIG)` against the extracted prebuilt tree;
   link target **`slang::slang`** (Linux: `libslang-compiler.so.0.2026.14.1`; Windows:
   `slang-compiler.lib` + `slang-compiler.dll`). Never link `slang::gfx` (deprecated
   upstream) and never ship `slang-llvm` (84 MB CPU-JIT backend, unused for SPIR-V)
   [R:A1, R:E3]. The Windows import-lib is MSVC-built; linking it from the zig/LLD
   toolchain is expected to work (COM-lite ABI: pure vtables, no STL/exceptions crossing)
   but MUST be empirically smoke-tested as the first Windows-target step, not assumed
   [R:A6, R:D2].
2. **Compilation API:** one process-lifetime `IGlobalSession` behind a mutex (global
   sessions are NOT thread-safe; distinct global sessions may run in parallel but each
   re-pays stdlib load). Session per target config; `loadModuleFromSource` → find entry
   points → `createCompositeComponentType` → `link` → `getEntryPointCode` → SPIR-V blob.
   Diagnostics are `IBlob`s on every step — always surfaced through `rx_core` logging,
   never swallowed [R:A2, R:A4, R:A6].
3. **SPIR-V floor:** pin `-capability spirv_1_3`-equivalent in the `TargetDesc`
   (explicit floor matching Vulkan 1.3 / Steam Deck reality) [R:A5]. Dynamic rendering
   needs no shader-side special-casing [R:A5].
4. **Reflection:** Slang's `ProgramLayout` / `TypeLayoutReflection::getDescriptorSet*`
   API directly produces set/binding/count/type + `BindingType::PushConstant` ranges —
   no intermediate metadata format exists in this design [R:A3, R:E1].
5. **Bindless architecture:** one global descriptor set (set 0): unbounded
   update-after-bind arrays per resource class (sampled images, samplers, storage
   buffers), `PARTIALLY_BOUND | UPDATE_AFTER_BIND` binding flags, update-after-bind pool.
   Per-draw identity = `uint32` indices in push constants, budgeted within the
   **128-byte guaranteed push-constant floor** (Steam Deck reports exactly 128)
   [R:B2, R:B3]. Every descriptor-indexing feature bit is runtime-queried and enabled
   explicitly at device creation (Roadmap-2022 hardware assumption justifies the design;
   the query justifies the runtime) [R:B1]. All bits confirmed present on Steam Deck
   RADV [R:B2].
6. **NonUniformResourceIndex discipline:** RDNA2 reports `...Native = false` — nonuniform
   indexing works but costs real re-convergence. Use `NonUniformResourceIndex()` ONLY
   where the index genuinely varies within a draw; a per-draw push-constant index is
   uniform and must not be wrapped. Slang has two open SPIR-V-decoration bugs in this
   area — any shader actually using nonuniform indexing gets `spirv-val` checked in its
   task [R:B2, R:B3, R:E4].
7. **Upload path:** VMA `AUTO + HOST_ACCESS_SEQUENTIAL_WRITE + ALLOW_TRANSFER_INSTEAD +
   MAPPED` for the direct/ReBAR path, with detection of the fallback (memory-type check)
   and a real staging-buffer + `vkCmdCopyBuffer*` path when it falls back. Single
   graphics-queue submission (correct everywhere, ideal on the Deck's unified memory)
   [R:C1].
8. **Mips:** `vkCmdBlitImage` chain with per-level barriers; check
   `VK_FORMAT_FEATURE_BLIT_DST_BIT` for the format and skip mips (single level) with a
   logged warning if unsupported. Samples use UNORM formats where linear-average blits
   are acceptable; the sRGB-mip-correctness caveat is documented in code where the blit
   chain lives [R:C2].
9. **Deferred destruction:** a `DeletionQueue` keyed on frame fences — resources
   retired while potentially referenced by in-flight command buffers are destroyed only
   after their frame's fence signals. Required for streaming eviction correctness; used
   by all samples' teardown.
10. **Texture loading:** stb_image (header-only, FetchContent). Public upload surface is
    "pixels + format + extent in → bindless handle out" so a libktx backend can slot in
    later without touching descriptor code [R:C3].
11. **Redistribution:** samples doing runtime compilation ship
    `libslang-compiler.so*` / `slang-compiler.dll` PLUS the runtime-loaded plugin libs
    (`slang-glslang`, `slang-glsl-module`, `slang-rt`) next to the executable
    (RPATH `$ORIGIN` on Linux; exe-adjacent on Windows). Exclude `slang-llvm` and `gfx`.
    Samples using only precompiled SPIR-V ship no Slang libs at all. The Vulkan loader
    is never bundled [R:D2]. Slang is Apache-2.0 w/ LLVM-exception; ship its LICENSE
    alongside redistributed binaries [R:A6].
12. **Slang fetch rework:** version-keyed fetch markers (fixes the Phase 1 Task 4
    deferred finding — bumping the pin must invalidate the old extraction), plus fetching
    the Windows archive when targeting Windows (import lib + DLLs are target-side needs,
    unlike host-side `slangc`) [R:A1, ledger ruling].

## Components (new)

| Component | Responsibility | Depends on |
|---|---|---|
| `rx_shader` (static lib) | Slang global-session/session lifecycle, compile source→SPIR-V with diagnostics, reflection walk → `ShaderLayoutInfo` (set layouts + push ranges + stage info) | `slang::slang`, `rx_core` |
| `rx_rhi_vk` additions | `PipelineLayoutBuilder` (ShaderLayoutInfo → VkDescriptorSetLayout/VkPipelineLayout, cached), `BindlessTable` (global set, index allocation via `rx_core::HandlePool`, update-after-bind writes), `Uploader` (staging ring + direct path), `Texture2D`/`MeshBuffers` creation, `DeletionQueue` | Phase 1 RHI, VMA |
| `samples/02_hotreload` | Runtime-compiled fullscreen shader, mtime-poll reload, pipeline recreate on change | `rx_shader` + Phase 1 |
| `samples/03_bindless_mesh` | Procedural textured meshes, reflection-derived layouts, bindless indices via push constants | everything above |
| `samples/04_streaming` | Live upload/evict through the bindless table, deletion-queue-protected | everything above |

Device gains descriptor-indexing feature enablement at creation (`VkPhysicalDeviceVulkan12Features`
chain via vk-bootstrap's `set_required_features_12`), and FrameSync integration points for
the deletion queue (per-frame fence handoff).

## Error handling

- Compile failures return diagnostics-bearing results (never aborts); hot-reload keeps
  the last good pipeline on failure and logs the diagnostic blob.
- Bindless table exhaustion / feature-missing at device creation are hard, loud failures
  at startup, not runtime surprises.
- Upload fallback (no ReBAR) is detected and logged once, not per-upload.
- All samples keep the Phase 1 bar: zero validation errors (modulo the documented layer
  false-positive guard), spec-valid usage only.

## Testing

- `rx_shader` tests: compile a known-good shader from string → valid SPIR-V magic +
  nonzero size; compile a known-bad shader → failure with non-empty diagnostics;
  reflection of a shader with a texture array + push constants → exact expected
  set/binding/type/count/range values.
- RHI tests (headless): BindlessTable create/write/free with validation clean;
  Uploader round-trip (upload → GPU copy back → byte compare); mip-chain generation
  produces expected level count; DeletionQueue destroys only after fence signal
  (stress: retire while in-flight, assert no validation error and no premature destroy).
- Sample headless gates: each sample has a no-window-interaction mode that renders
  N frames offscreen/readback, asserts pixels (hotreload: recompile actually changes
  output color between frame batches), exits 0/1 for ctest.
- CI: all of the above on linux-native (lavapipe); windows-cross-zig builds everything
  + wine-runs the non-GPU test set; sample binaries + required Slang runtime libs
  uploaded as artifacts.

## Phase exit

`v0.2.0-phase2` GitHub release: all three samples for Linux + Windows with required
runtime libs and run instructions; README updated; final whole-branch review clean.
