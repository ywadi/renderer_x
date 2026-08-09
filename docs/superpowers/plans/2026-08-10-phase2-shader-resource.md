# Phase 2 Implementation Plan: Slang Runtime Shader System + Resource Management

> **For agentic workers:** Executed via subagent-driven development. Each task section is a self-contained brief. Required reading for every task: the spec (`docs/superpowers/specs/2026-08-10-phase2-shader-resource-design.md`) and the cited sections of the research findings (`.superpowers/sdd/2026-08-10-phase1-completion/phase2-research-findings.md` — in-repo, citation-backed; [R:X] refs point there).

**Goal:** In-process Slang compilation + reflection driving pipeline layouts, bindless resource management with a real upload path, and three deployed samples proving it end to end.

**Prerequisite:** Phase 1 complete (all tasks of `2026-08-10-phase1-completion.md`, including FrameSync and the samples/CI/deployment structure).

## Global Constraints

- Everything from the Phase 1 completion plan's Global Constraints applies verbatim (both presets always clean, zero validation errors, dep pins, sub-minute warm builds, no AI attribution — verified directly per task, no placeholders, ready-made libs first).
- The spec's **Fixed decisions** section is binding. Deviations require coordinator sign-off before implementation, not after.
- Push-constant usage in any shader/sample stays within 128 bytes total [R:B2].
- `NonUniformResourceIndex()` only where an index varies within a draw; never on per-draw push-constant indices. Any shader using it gets `spirv-val` run on its SPIR-V in that task [R:B3, R:E4].
- Slang API calls follow the verified shapes in [R:A2]/[R:A3] — if the shipped `slang.h` disagrees with the research file, the header wins; note the discrepancy in the report.
- **vk-bootstrap landmine (verified in Phase 1 Task 1's review):** the pinned vk-bootstrap caches instance-level Vulkan function pointers process-wide from the FIRST `vkb::Instance` built in a process and never refreshes them — building a headless/narrow-extension instance before a windowed/broader one poisons later physical-device selection (SIGSEGV). Any NEW test binary in this phase that constructs Vulkan devices (rx_shader tests, sample headless gates) must either (a) warm the cache at binary startup by building-and-destroying a Context with the broadest extension set the binary will ever use (the pattern `src/rx_rhi_vk/tests/doctest_main.cpp` establishes — copy it), or (b) keep headless-only and windowed instance construction in separate test executables. See the doc comment on `Context::create`.

---

### Task 1: Slang runtime linking + ShaderCompiler (rx_shader)

**Files:**
- Modify: `tools/fetch_slang.cmake` (version-keyed markers; fetch Windows archive `slang-2026.14.1-windows-x86_64.tar.gz` in addition to the Linux one when the target preset is Windows — the import lib/DLLs are target-side, `slangc` stays host-side; expose `slang_ROOT`/`CMAKE_PREFIX_PATH` for the package)
- Create: `src/rx_shader/include/rx_shader/compiler.h`, `src/rx_shader/src/compiler.cpp`, `src/rx_shader/CMakeLists.txt`, `src/rx_shader/tests/compiler_test.cpp` (+ `doctest_main.cpp`)
- Modify: root `CMakeLists.txt` (`add_subdirectory(src/rx_shader)`)

**Interfaces produced:**
- `rx::shader::Compiler` — `static create() -> std::optional<Compiler>` (one mutex-guarded process-lifetime `IGlobalSession` [R:A4/A6]); `compileFromSource(moduleName, source, entryPointNames[]) -> CompileResult`; `compileFromFile(path, entryPointNames[]) -> CompileResult`.
- `rx::shader::CompileResult { bool ok; std::vector<SpirvBlob> entryPointCode; std::string diagnostics; }` where `SpirvBlob { std::vector<uint32_t> code; VkShaderStageFlagBits stage; std::string entryPointName; }`. Diagnostics ALWAYS captured (warnings on success too) and logged via `RX_LOG_WARN`/`RX_LOG_ERROR` [R:A2].
- The linked `slang::IComponentType` is retained inside `CompileResult` (opaque member) for Task 2's reflection.

**Key steps:**
1. Rework the fetch: marker file becomes `.rx-fetched-<version>` (old unversioned marker → treat as stale, re-fetch); on Windows-target configure, additionally fetch/extract the Windows archive into `third_party/slang-prebuilt/windows-x86_64/`. The CMake package lives at `lib/cmake/slang/` (Linux) vs top-level `cmake/` (Windows) [R:A1] — point `find_package(slang CONFIG REQUIRED)` at the right prefix per target. Link `slang::slang` ONLY (never `slang::gfx` [R:E3]).
2. **Windows link smoke test FIRST** (de-risk before building the API): a trivial TU calling `slang::createGlobalSession` cross-compiled via the `windows-cross-zig` preset must link against the MSVC-built `slang-compiler.lib` through zig/LLD [R:A6/D2 — expected to work, must be proven]. If it fails to link, STOP and report BLOCKED with the exact linker error — this is a plan-level risk gate, do not improvise workarounds.
3. Implement Compiler per [R:A2]: `TargetDesc{format=SLANG_SPIRV, profile=findProfile(...)}` with an explicit SPIR-V 1.3 capability floor [R:A5]; session reuse; full diagnostics plumbing.
4. Tests (linux-native, real execution): known-good vertex+fragment source string → 2 SPIR-V blobs with magic `0x07230203`, correct stages; deliberately-broken source → `ok=false` + non-empty diagnostics containing the error line; second compile through the same Compiler reuses the global session (no crash, sane timing).
5. Runtime lib placement for executables that link `rx_shader`: Linux RPATH `$ORIGIN` + copy `libslang-compiler.so*` + plugin libs (`slang-glslang`, `slang-glsl-module`, `slang-rt`) next to test/sample binaries at build time (CMake `add_custom_command` copy step); Windows: DLLs copied exe-adjacent. Exclude `slang-llvm`/`gfx` [R:D2]. `rx_shader_tests` must actually RUN from the build tree with this mechanism (that's the proof it works).

**Verify:** full ctest green on linux-native incl. new tests; `windows-cross-zig` configures+builds incl. the link smoke test binary; both presets' shader compilation (Phase 1 `slangc` path) still works; commit clean (no AI attribution, verified).

---

### Task 2: Reflection → descriptor set layouts + pipeline layouts

**Files:**
- Create: `src/rx_shader/include/rx_shader/reflection.h`, `src/rx_shader/src/reflection.cpp`, `src/rx_shader/tests/reflection_test.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/pipeline_layout.h`, `src/rx_rhi_vk/src/pipeline_layout.cpp`, `src/rx_rhi_vk/tests/pipeline_layout_test.cpp`
- Modify: both CMakeLists

**Interfaces produced:**
- `rx::shader::ShaderLayoutInfo { struct Binding { uint32_t set, binding, count; VkDescriptorType type; VkShaderStageFlags stages; bool unboundedArray; }; std::vector<Binding> bindings; struct PushRange { VkShaderStageFlags stages; uint32_t offset, size; }; std::vector<PushRange> pushRanges; }`
- `rx::shader::reflect(const CompileResult&) -> std::optional<ShaderLayoutInfo>` — walks `ProgramLayout`/`TypeLayoutReflection::getDescriptorSet*` + `BindingType::PushConstant` per [R:A3]; maps `slang::BindingType` → `VkDescriptorType`; merges per-entry-point stage flags.
- `rx::rhi::PipelineLayoutBuilder` — `build(VkDevice, const ShaderLayoutInfo&) -> std::optional<PipelineLayoutBundle { std::vector<VkDescriptorSetLayout> setLayouts; VkPipelineLayout layout; }>`; unbounded-array bindings get `UPDATE_AFTER_BIND | PARTIALLY_BOUND` flags + update-after-bind layout flag (consumed by Task 3's bindless set). RAII bundle owns its handles.

**Tests:** reflect a shader with a `Texture2D[] ` unbounded array + a sampler + a `ConstantBuffer` + push constants → assert exact set/binding/type/count/stage/range values (hand-computed expected table in the test); build the pipeline layout on a headless device → non-null handles, zero validation errors; a shader with >128-byte push constants → reflection succeeds but `PipelineLayoutBuilder` rejects with a logged error (enforces the budget [R:B2]).

**Verify:** full ctest green; both presets build; commit clean.

---

### Task 3: Device descriptor-indexing enablement + BindlessTable

**Files:**
- Modify: `src/rx_rhi_vk/src/device.cpp` (+header if needed): chain `VkPhysicalDeviceVulkan12Features` via vk-bootstrap `set_required_features_12` enabling exactly: `descriptorIndexing`, `runtimeDescriptorArray`, `descriptorBindingPartiallyBound`, `descriptorBindingVariableDescriptorCount`, `descriptorBindingSampledImageUpdateAfterBind`, `descriptorBindingStorageImageUpdateAfterBind`, `descriptorBindingStorageBufferUpdateAfterBind`, `descriptorBindingUpdateUnusedWhilePending`, `shaderSampledImageArrayNonUniformIndexing`, `shaderStorageBufferArrayNonUniformIndexing` [R:B1/B2 — all confirmed on Deck RADV]. Selection failure → loud startup error naming the missing feature.
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`, `src/rx_rhi_vk/src/bindless.cpp`, `src/rx_rhi_vk/tests/bindless_test.cpp`

**Interfaces produced:**
- `rx::rhi::BindlessTable` — `static create(VkDevice, capacities{sampledImages, samplers, storageBuffers}) -> std::optional<BindlessTable>`: ONE descriptor set (set 0) from an update-after-bind pool, bindings 0/1/2 as runtime arrays with `PARTIALLY_BOUND | UPDATE_AFTER_BIND | VARIABLE_DESCRIPTOR_COUNT` (last binding) per [R:B3]; `descriptorSetLayout()`, `descriptorSet()`; `registerSampledImage(VkImageView, VkImageLayout) -> BindlessHandle` (generational — reuse `rx::core::HandlePool` for index allocation), `registerSampler(VkSampler)`, `registerStorageBuffer(VkBuffer, range)`, `release(BindlessHandle)`; `BindlessHandle::index() -> uint32_t` (the shader-visible index).
- Writes happen immediately via `vkUpdateDescriptorSets` (update-after-bind makes this legal while bound, except for descriptors referenced by executing commands without `UPDATE_UNUSED_WHILE_PENDING` semantics — released slots are only rewritten after release, and release safety versus in-flight frames is Task 4's DeletionQueue's job; document this contract in the header).

**Tests (headless device):** create table (capacities 1024/16/256) → valid handles, zero validation errors; register/release/re-register cycles reuse indices with bumped generations; write a real image view + sampler + buffer and bind the set in a trivial dispatch-free command buffer (bind + no draw) → validation clean; feature-enablement failure path unit-tested by requesting an absurd capacity beyond `maxDescriptorSetUpdateAfterBindSampledImages` → clean error, no crash. Existing Phase 1 tests still green (Device change is additive).

**Verify:** full ctest green; both presets build; commit clean.

---

### Task 4: Uploader, DeletionQueue, Texture2D + mips, MeshBuffers

**Files:**
- Modify: `third_party/CMakeLists.txt` (stb via FetchContent — header-only, `stb_image.h`; same Populate+PARENT_SCOPE pattern as volk/VMA)
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/upload.h`, `src/rx_rhi_vk/src/upload.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h`, `src/rx_rhi_vk/src/deletion_queue.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/texture.h`, `src/rx_rhi_vk/src/texture.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/mesh_buffers.h`, `src/rx_rhi_vk/src/mesh_buffers.cpp`
- Create: tests for each (`upload_test.cpp`, `deletion_queue_test.cpp`, `texture_test.cpp`)

**Interfaces produced:**
- `rx::rhi::Uploader` — `create(Allocator&, Device&)`; `uploadToBuffer(dst VkBuffer, offset, data, size)`; `uploadToImage(dst Texture2D&, pixels, generateMips)`; direct path via `HOST_ACCESS_ALLOW_TRANSFER_INSTEAD` + fallback detection (check resulting memory-type properties once, log once) + staging path via ring buffer + `vkCmdCopyBuffer`/`vkCmdCopyBufferToImage` on the graphics queue [R:C1]. Synchronous flush API is acceptable this phase (`flush()` submits + fences); per-frame async batching is a later optimization — say so in the header.
- **Known API gap to close in this task (from Phase 1 Task 3's review):** `Allocator`/`Buffer` expose no flush/invalidate surface — the `VmaAllocation` is private, so `vmaFlushAllocation`/`vmaInvalidateAllocation` are unreachable, and Phase 1's readback test had to guard on device-wide host-coherence as a proxy. The Uploader work (readbacks, `ALLOW_TRANSFER_INSTEAD` paths, non-coherent memory) makes this load-bearing: add `Buffer::flush(offset,size)` / `Buffer::invalidate(offset,size)` (wrapping the VMA calls) and use them wherever mapped memory is read after GPU writes or written before GPU reads on possibly-non-coherent types.
- `rx::rhi::DeletionQueue` — `retire(std::function<void()>, uint64_t frameIndex)`; `onFrameFenceSignaled(frameIndex)` runs destructors whose frame completed; `flushAll(vkDeviceWaitIdle first)` for shutdown. Integrates with Phase 1 `FrameSync`'s frame indexing (add the minimal hook FrameSync needs — current frame counter accessor — if not already present).
- `rx::rhi::Texture2D` — VMA image + view, `create(Allocator&, extent, format, usage, mipLevels)`; mip generation via blit chain with per-level barriers, `VK_FORMAT_FEATURE_BLIT_DST_BIT` checked, unsupported → single mip + `RX_LOG_WARN` [R:C2]; sRGB-averaging caveat documented at the blit code.
- `rx::rhi::MeshBuffers` — device-local vertex+index buffers created through Uploader.

**Tests (headless):** buffer upload → GPU → copy back → byte-exact; image upload 64x64 with mips → readback level 0 exact, level count correct, validation clean; DeletionQueue: retire a buffer "used" by an in-flight (fence-unsignaled) frame, assert not destroyed until fence signal, then destroyed exactly once — and a stress loop (many retire/signal cycles) clean under validation.

**Verify:** full ctest green; both presets build; commit clean.

---

### Task 5: sample_02_hotreload (parallel-lane capable after Tasks 1-2)

**Files:** `samples/02_hotreload/main.cpp`, `CMakeLists.txt`, `shader/hotreload.slang` (installed next to binary), samples/README.md update. Root CMakeLists wiring.

Fullscreen triangle; fragment shader lives on disk next to the binary; poll mtime (~4Hz `stat`, no new deps [R:D1]); on change: `Compiler::compileFromFile` → on success build new pipeline (reflection-driven layout via Task 2) and swap (old pipeline retired via DeletionQueue if Task 4 is merged; otherwise `vkDeviceWaitIdle` swap is acceptable ONLY if Task 4 hasn't landed yet — coordinator will state which at dispatch); on failure keep last-good pipeline, log diagnostics, keep rendering.
Headless ctest mode: compile embedded source A → render 2 frames offscreen → readback color; then compile source B (different constant color) → render → readback differs as expected → exit 0. Present mode: window + live editing.
Ships Slang runtime libs per the Task 1 mechanism; this sample is the redistribution proof.

**Verify:** headless gate green in ctest; `--present` manually verified rendering + a live reload on this machine; both presets build; commit clean.

---

### Task 6: sample_03_bindless_mesh

**Files:** `samples/03_bindless_mesh/**` (+ shader), README update, root wiring.

Procedural geometry (cube, sphere, plane — generated in code, no importer), 4 distinct generated textures (checkerboards/gradients via stb-independent procedural fill; stb_image still used to load one real PNG embedded in the sample dir to prove the path), uploaded via Uploader into BindlessTable; per-draw push constants = {mvp offset index or transform, textureIndex, samplerIndex} within 128 bytes; descriptor set layout + pipeline layout come from `reflect()` on the actual sample shader — hand-typed layouts are forbidden (that's the point of the sample). Uniform per-draw index → NO `NonUniformResourceIndex` [R:B3/E4]. Depth buffer via Texture2D. Camera orbits in present mode.
Headless gate: render one frame offscreen 256x256, assert ≥3 distinct texture samples appear at probe pixels (known geometry positions), validation clean, exit codes.

**Verify:** headless gate in ctest; present mode verified; both presets; commit clean.

---

### Task 7: sample_04_streaming

**Files:** `samples/04_streaming/**`, README update, root wiring.

24 procedurally-generated textures, a resident budget of 8 bindless slots; each second (or every N frames headlessly) the next texture streams in through Uploader and the oldest resident is evicted: `BindlessTable::release` + DeletionQueue-retired destruction keyed on the frame fence — the eviction-while-in-flight safety is the entire point [R:D1 sample 3]. Grid of quads each drawing its texture if resident (partially-bound: non-resident slots must not be sampled — draw skips them; document why PARTIALLY_BOUND makes the set valid anyway).
Headless gate: run 60 frames, assert the full rotation happened (every texture was resident at some point — track via readback probes at grid positions on selected frames), zero validation errors (this catches premature-destroy bugs), exit codes.

**Verify:** headless gate in ctest; present mode verified; both presets; commit clean.

---

### Task 8: CI + packaging for Phase 2 samples

**Files:** `.github/workflows/ci.yml` (extend), `samples/README.md` finalize.

Linux job: all new tests + all three sample headless gates under xvfb/lavapipe (same investigate-don't-skip rule as Phase 1). Windows job: everything builds; wine-run the non-GPU test set (rx_shader compiler tests run under wine ONLY if the Slang Windows DLLs load under wine — investigate; if they don't, exclude with an explicit workflow comment, never silently). Artifacts: all three samples per platform INCLUDING the Slang runtime libs + LICENSE for the hotreload sample (and any other sample that links rx_shader), laid out exactly as a user would unzip-and-run them [R:D2]. Budget check still passes with the grown codebase — if the warm build now exceeds 60s, report it (coordinator decides budget adjustment vs optimization; do not silently raise the number).
Push and `gh run watch` to green — a red run gets fixed in-task.

**Verify:** both jobs green on GitHub for real; artifact zips manually spot-checked (download one, run it on this machine). Commit clean.

---

### Task 9: Phase 2 close-out (coordinator)

1. Final whole-branch review (Sonnet) over Phase 2's full range, pointed at both ledgers' deferred/parked items — triage before release.
2. Fix wave if needed (one dispatch + one scoped re-review).
3. `gh release create v0.2.0-phase2` — all three samples × both platforms with runtime libs + licenses + run instructions; release notes; README updated (Haiku doc dispatch, reviewed).
4. Ledgers closed; Phase 2 marked complete. **This ends the engagement's committed scope (Phase 1 + Phase 2).**

---

## Execution order, model tiers, parallelism

| Order | Task | Model | Lane |
|---|---|---|---|
| 1 | T1 Slang linking + Compiler | Sonnet | main |
| 2 | T2 Reflection + layouts | Sonnet | main |
| 3 | T3 Bindless | Sonnet | main |
| 3 (parallel) | T5 hotreload sample | Sonnet | worktree (needs only T1+T2; files disjoint from T3/T4) |
| 4 | T4 Upload/Deletion/Texture/Mesh | Sonnet | main |
| 5 | T6 bindless_mesh sample | Sonnet | main (after T5 merged) |
| 6 | T7 streaming sample | Sonnet | main |
| 7 | T8 CI + packaging | Sonnet | main |
| 8 | T9 close-out | coordinator (+Sonnet review, +Haiku README) | main |

Every task: Sonnet review, coordinator commit-hygiene check, ledger entry, both presets clean.
