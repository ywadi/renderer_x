# Phase 2 Research Findings: Slang Runtime Compilation, Bindless, Resource Management, Samples

Date: 2026-08-10
Method: (1) direct extraction/inspection of the actual Slang v2026.14.1 release archives
(downloaded and unpacked `slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz` and
`slang-2026.14.1-windows-x86_64.tar.gz` from the real GitHub release), plus the shipped
`slang.h` header, CMake package files, and stdlib source; (2) live queries against
`vulkan.gpuinfo.org`, GitHub API/raw sources, and Khronos docs. Every claim below is tagged
`[verified: URL]` or `[verified: local extraction of <archive> from <URL>]`.

---

## A. Slang runtime compilation (in-process)

### A1. Exact prebuilt archive contents (v2026.14.1)

Release: `https://github.com/shader-slang/slang/releases/tag/v2026.14.1`, published
2026-07-30T06:48:50Z. Confirmed via GitHub API
(`api.github.com/repos/shader-slang/slang/releases/tags/v2026.14.1`) that this tag is real
and has 34 assets. [verified: https://github.com/shader-slang/slang/releases/tag/v2026.14.1]

**Linux x86_64** — three Linux x86_64 variants exist: unlabeled (`slang-2026.14.1-linux-x86_64.tar.gz`,
77 MB — statically bundles `libslang-llvm.so`-equivalent for CPU/host targets), `-glibc-2.27`
(23.6 MB) and `-glibc-2.28` (23.7 MB). Phase 1's `tools/fetch_slang.cmake` targets the
`-glibc-2.27` variant. I downloaded and fully extracted it (3,905 files). Layout:
```
bin/slang, bin/slangc, bin/slangd, bin/slangi     (executables; slangc = CLI compiler)
bin/gfx.slang, bin/slang.slang                    (reference stdlib source, informational)
include/slang.h                                   (232,971 bytes — the C++/C API, single header)
include/slang-com-ptr.h, slang-com-helper.h        (COM smart-pointer helpers, ComPtr<T>)
include/slang-gfx.h                                (deprecated GFX layer — see below, do not use)
include/slang-cpp-*.h, slang-cuda-prelude.h, ...   (host/CUDA/torch code-gen preludes, not needed for SPIR-V)
lib/libslang-compiler.so.0.2026.14.1 (33.6 MB)     (the real compiler; libslang.so and
                                                     libslang-compiler.so are symlinks to it)
lib/libslang-rt.so.0.2026.14.1 (917 KB)            (companion runtime library)
lib/libslang-glslang-2026.14.1.so (10.0 MB)        (glslang-based downstream/legacy plugin)
lib/libslang-glsl-module-2026.14.1.so (1.4 MB)     (the "glsl" compatibility stdlib module,
                                                     loaded when a shader does `import glsl;`)
lib/slang-standard-module-2026.14.1/               (precompiled Slang stdlib modules, incl.
                                                     slang/bindless-storage.slang — see B3)
lib/cmake/slang/slangConfig.cmake, slangTargets*.cmake, slangConfigVersion.cmake
lib/pkgconfig/slang-compiler.pc
LICENSE, LICENSES/{Apache-2.0,BSL-1.0,CC-BY-4.0,LicenseRef-UOI-NCSA,LLVM-exception,MIT,Unlicense}.txt
```
[verified: local extraction of slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz from
https://github.com/shader-slang/slang/releases/download/v2026.14.1/slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz]

**Windows x86_64** (`slang-2026.14.1-windows-x86_64.tar.gz`, 53.8 MB) — different layout, all
binaries in `bin/`, import libs in `lib/`, CMake package at top-level `cmake/` (not `lib/cmake/slang/`):
```
bin/slang-compiler.dll (25.3 MB)   lib/slang-compiler.lib (168 KB)   <- what "slang::slang" actually links
bin/slang.dll (159 KB)             lib/slang.lib (164 KB)            <- present but NOT referenced by
                                                                          any generated CMake target; do not rely on it
bin/slang.exe, slangc.exe, slangd.exe, slangi.exe
bin/slang-glslang.dll (6.2 MB), slang-glsl-module.dll (1.5 MB), slang-rt.dll (311 KB)
bin/slang-llvm.dll (84.4 MB)       <- CPU/host JIT backend, NOT needed for SPIR-V-only use; exclude from redistribution
bin/gfx.dll, lib/gfx.lib           <- deprecated GFX layer, do not use (see A2 note)
cmake/slangConfig.cmake, slangTargets.cmake, slangTargets-release.cmake
```
[verified: local extraction of slang-2026.14.1-windows-x86_64.tar.gz from
https://github.com/shader-slang/slang/releases/download/v2026.14.1/slang-2026.14.1-windows-x86_64.tar.gz]

**CMake integration (exact, from the shipped `slangTargets-release.cmake`, not guessed):**
`find_package(slang CONFIG REQUIRED)` (point `CMAKE_PREFIX_PATH`/`slang_ROOT` at the extracted
tree) exposes target **`slang::slang`** — `SHARED IMPORTED`, `INTERFACE_INCLUDE_DIRECTORIES`
= `<prefix>/include`, `INTERFACE_COMPILE_DEFINITIONS` = `SLANG_DYNAMIC`.
- Linux: `IMPORTED_LOCATION` = `lib/libslang-compiler.so.0.2026.14.1`, `IMPORTED_SONAME` same.
- Windows: `IMPORTED_IMPLIB` = `lib/slang-compiler.lib`, `IMPORTED_LOCATION` = `bin/slang-compiler.dll`.
`target_link_libraries(your_target PRIVATE slang::slang)` is sufficient; no manual `-l`/paths
needed once the package is found. Also exposed: `slang::slangc` (executable target, for
build-time shader compilation via `add_custom_command`), `slang::gfx` (deprecated — Slang's own
README states *"GFX is being deprecated in favor of [slang-rhi](https://github.com/shader-slang/slang-rhi)"* —
do not use it; this project already has its own `rx_rhi_vk`), and MODULE-IMPORTED plugin
targets `slang::slang-glslang` / `slang::slang-glsl-module` that are not meant to be linked
directly — they're runtime-loaded plugins (see A6/D2 for redistribution implications).
[verified: local extraction, `lib/cmake/slang/slangTargets-release.cmake` (Linux) and
`cmake/slangTargets-release.cmake` (Windows) from the same archives; README quote from
https://github.com/shader-slang/slang/blob/master/README.md]

### A2. Current C++ API (verified against the actual shipped `slang.h`, not paraphrased docs)

```cpp
// 1. Global session (create once, reuse for the process lifetime — see A4)
Slang::ComPtr<slang::IGlobalSession> globalSession;
slang::createGlobalSession(globalSession.writeRef());   // inline wrapper around slang_createGlobalSession2

// 2. Session with a SPIR-V target
slang::TargetDesc targetDesc = {};
targetDesc.format  = SLANG_SPIRV;
targetDesc.profile = globalSession->findProfile("glsl_450"); // or "sm_6_x" — both accepted, see A5
slang::SessionDesc sessionDesc = {};
sessionDesc.targets = &targetDesc;
sessionDesc.targetCount = 1;
Slang::ComPtr<slang::ISession> session;
globalSession->createSession(sessionDesc, session.writeRef());   // ISession::createSession, in slang.h:4028

// 3. Load a module (from source string or from a file path Slang resolves via -I search paths)
Slang::ComPtr<slang::IBlob> diagnostics;
slang::IModule* module = session->loadModule("MyShaders", diagnostics.writeRef());          // slang.h:4447
// or: session->loadModuleFromSource(moduleName, path, sourceBlob, diagnostics.writeRef());  // slang.h:4451

// 4. Find entry point(s) and compose
Slang::ComPtr<slang::IEntryPoint> entryPoint;
module->findEntryPointByName("main", entryPoint.writeRef());                                 // slang.h:5570
slang::IComponentType* parts[] = { module, entryPoint };
Slang::ComPtr<slang::IComponentType> program;
session->createCompositeComponentType(parts, 2, program.writeRef());                         // slang.h:4486

// 5. Link
Slang::ComPtr<slang::IComponentType> linkedProgram;
Slang::ComPtr<slang::IBlob> linkDiagnostics;
program->link(linkedProgram.writeRef(), linkDiagnostics.writeRef());

// 6. Get SPIR-V blob (entryPointIndex within linkedProgram, targetIndex = 0)
Slang::ComPtr<slang::IBlob> spirvBlob, codeDiagnostics;
linkedProgram->getEntryPointCode(0, 0, spirvBlob.writeRef(), codeDiagnostics.writeRef());     // slang.h:5360
// spirvBlob->getBufferPointer() / getBufferSize() is the raw SPIR-V, ready for vkCreateShaderModule.
```
Diagnostics from every step above are `IBlob*` — non-null means there is diagnostic text
(warnings and/or errors); check the returned `SlangResult` for hard failure, and always read
the blob (`(const char*)blob->getBufferPointer()`) for human-readable messages regardless of
success, since warnings surface the same way.
[verified: local extraction, `include/slang.h` lines 4028, 4447-4453, 4486-4490, 5360-5364, 5570,
6038-6047 from slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz; cross-checked structurally against
https://raw.githubusercontent.com/shader-slang/slang/master/docs/user-guide/08-compiling.md]

### A3. Reflection API (verified against `slang.h`; this is what drives `VkDescriptorSetLayout`/`VkPipelineLayout` generation)

```cpp
slang::ProgramLayout* layout = linkedProgram->getLayout(0 /*targetIndex*/);

// Entry points
unsigned n = layout->getEntryPointCount();
slang::EntryPointReflection* ep = layout->getEntryPointByIndex(i);
SlangStage stage = ep->getStage();                    // maps to VK_SHADER_STAGE_*
unsigned paramCount = ep->getParameterCount();
slang::VariableLayoutReflection* p = ep->getParameterByIndex(j);

// Global scope (module-level resources not tied to one entry point — where a bindless
// global descriptor set naturally lands)
slang::TypeLayoutReflection* globalTypeLayout = layout->getGlobalParamsTypeLayout();
slang::VariableLayoutReflection* globalVarLayout = layout->getGlobalParamsVarLayout();

// Per-parameter binding location
unsigned bindingIndex = p->getBindingIndex();          // -> binding=
unsigned bindingSpace = p->getBindingSpace();          // -> set= (descriptor set index)
slang::ParameterCategory cat = p->getCategory();        // e.g. PushConstantBuffer, DescriptorTableSlot

// Deriving descriptor sets/ranges directly (walks a TypeLayoutReflection, e.g. from
// globalTypeLayout, or from a ParameterBlock<T>'s element type layout):
SlangInt setCount = typeLayout->getDescriptorSetCount();
for (SlangInt s = 0; s < setCount; ++s) {
    SlangInt rangeCount = typeLayout->getDescriptorSetDescriptorRangeCount(s);
    for (SlangInt r = 0; r < rangeCount; ++r) {
        slang::BindingType bindingType =
            typeLayout->getDescriptorSetDescriptorRangeType(s, r);      // Texture/Sampler/ConstantBuffer/PushConstant/...
        SlangInt count = typeLayout->getDescriptorSetDescriptorRangeDescriptorCount(s, r);
        SlangInt indexOffset = typeLayout->getDescriptorSetDescriptorRangeIndexOffset(s, r);
        // -> directly build VkDescriptorSetLayoutBinding{ .binding = indexOffset,
        //      .descriptorType = map(bindingType), .descriptorCount = count, ... }
    }
}
```
`slang::ParameterCategory` (enum, `slang.h:2638`) includes `ConstantBuffer`, `ShaderResource`,
`UnorderedAccess`, `SamplerState`, `DescriptorTableSlot`, `PushConstantBuffer`, `RegisterSpace`.
`slang::BindingType` (enum, `slang.h:2680`) includes `Sampler`, `Texture`, `ConstantBuffer`,
`ParameterBlock`, `TypedBuffer`, `RawBuffer`, `CombinedTextureSampler`,
`RayTracingAccelerationStructure`, and **`PushConstant`** — push constants are identified
unambiguously via `BindingType::PushConstant` / `ParameterCategory::PushConstantBuffer`, giving
size (`typeLayout->getSize()`) and offset directly, enough to build
`VkPushConstantRange{ .stageFlags = map(stage), .offset, .size }` and merge overlapping ranges
across stages into a single `VkPipelineLayout`.
[verified: local extraction, `include/slang.h` lines 2638-2698, 3007-3086, 3359-3382, 3554-3574,
3821-3829 from the same archive]

### A4. Hot-reload guidance (cheap recompiles, session reuse)

Directly from the doc-comment above `IGlobalSession` in the shipped header: *"An application
may create and re-use a single global session across multiple sessions, in order to amortize
startup costs (in current Slang this is mostly the cost of loading the Slang standard
library)."* Practical hot-reload pattern: create **one `IGlobalSession` for the process
lifetime**; on each file-watcher trigger, create a fresh (cheap) `ISession` only if target
config changed, otherwise reuse it; call `loadModuleFromSource`/`loadModule` with the new
source, recompose entry points, link, and re-extract SPIR-V. Front-end steps (module load,
specialization, linking) "still require external synchronization unless documented
otherwise" — i.e. not safe to call concurrently on the same session/global-session from
multiple threads without your own mutex (see A6). Backend codegen
(`getEntryPointCode`/`getTargetCode`/`getResultAsFileSystem`/`getTargetMetadata`/
`getEntryPointMetadata`) is explicitly called out as an **"experimental" concurrent-safe**
path once a component type is fully linked — i.e. you may generate code for several already-linked
`IComponentType`s from multiple threads, but that's a narrower guarantee than "the whole
pipeline is thread-safe." There's also an experimental
`IModulePrecompileService_Experimental::precompileForTarget` for embedding precompiled target
IR in a module (useful for shipping precompiled shaders with fast incremental link), and a
`SLANG_UNIT_TEST_DUMP...`-adjacent compiler option `UseUpToDateBinaryModule` to skip recompilation
when a cached binary module is current — both marked experimental, so treat as an optimization
to add later, not the baseline hot-reload path for Phase 2.
[verified: local extraction, `include/slang.h` lines 4014-4020, 5346-5359, 5633-5640, 1127 from
the same archive]

### A5. SPIR-V version emitted vs Vulkan 1.3 compatibility; dynamic-rendering constraints

`slangc -h target` lists `spirv: SPIR-V binary` as a first-class target; `slangc -h capability`
lists explicit version-selection capabilities `spirv_1_0` through `spirv_1_6` ("minimum
supported SPIR-V version") and matching `glsl_spirv_1_x` variants, selected via `-capability`
(or the C++ `TargetDesc`/`-profile+capability` combination). The `-profile` values accepted
today are still the D3D-shader-model-style tokens `sm_{4_0..6_10}` and GLSL-style
`glsl_{150..460}` — **`-profile sm_6_0`**, the exact flag Phase 1's `task-4-brief.md` assumed
for `slangc`, is confirmed to still be a currently-accepted value in v2026.14.1's actual `-h`
output; the profile governs feature/shader-model availability, not the emitted SPIR-V version
directly. Vulkan 1.3 requires implementations to accept SPIR-V up to version 1.6 (and, being
additive, all older versions 1.0-1.5 remain valid); Slang's default output for a `spirv`
target without an explicit `spirv_1_x` capability floor targets a version compatible with the
requested profile/capabilities, so pinning `-capability spirv_1_3` (matching the Vulkan 1.3
floor and RDNA2/Steam Deck reality) or leaving it to Slang's default and letting validation
layers catch any mismatch are both viable; explicit is safer for a fixed baseline.
**Dynamic rendering imposes no SPIR-V/shader-side constraint at all** — `VK_KHR_dynamic_rendering`
(core in 1.3) only changes pipeline creation (`VkPipelineRenderingCreateInfo` in
`VkGraphicsPipelineCreateInfo::pNext` instead of a `VkRenderPass`) and command recording
(`vkCmdBeginRendering`/`vkCmdEndRendering`); shader code and its reflected bindings are
identical either way, so nothing in the Slang↔reflection↔pipeline-layout pipeline needs to
special-case it.
[verified: local extraction, `slangc -h target` and `slangc -h capability` output from
slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz's `bin/slangc`; Vulkan SPIR-V version
requirement verified: https://docs.vulkan.org/spec/latest/appendices/spirvenv.html]

### A6. Threading, glibc compatibility, ABI/API stability, licensing

**Threading (from `slang.h` doc comments, verbatim):** *"A single global session object is
currently *not* thread-safe. Unless documented otherwise, a global session and the objects
created from it should be externally synchronized when shared across threads. Distinct global
sessions may be used from different threads in parallel."* Practical rule: one mutex-guarded
`IGlobalSession` (or one per worker thread if you truly need parallel independent compiles —
each pays the stdlib-load cost again), front-end operations (load/link/specialize) externally
synchronized, backend codegen on already-linked component types safe to parallelize
("experimental").
[verified: local extraction, `include/slang.h` lines 4016-4019]

**glibc compatibility:** measured directly (`objdump -T`) against the extracted
`libslang-compiler.so.0.2026.14.1` and `bin/slangc`: the highest glibc symbol version actually
referenced is **`GLIBC_2.17`** (2013) — looser than what the `-glibc-2.27` filename suggests
(that name reflects the build sysroot baseline they compiled against, not the true symbol
floor). `DT_NEEDED` for `slangc`/`slangd`/`slangi` is `libslang-compiler.so.0.2026.14.1` +
`libpthread.so.0`/`libstdc++.so.6`/`libm.so.6`/`libgcc_s.so.1`/`libc.so.6` only — the plugin
`.so`s (`libslang-glslang-*`, `libslang-glsl-module-*`) and `libslang-rt.so` are **not**
`DT_NEEDED` by the main compiler library, meaning they're loaded on demand (dlopen-style
plugins), not hard link-time dependencies — but they must still ship alongside
`libslang-compiler.so` in the same directory for those code paths (legacy/glslang-backed
targets, `import glsl;`) to work at runtime (see D2). Separately, I compiled trivial C with
this repo's own `toolchain/zig/zig cc -target x86_64-linux-gnu` (zig 0.16.0, the pinned
toolchain) and measured its glibc floor: **`GLIBC_2.2.5`** — i.e. zig's default (unversioned)
Linux target is *more* backward-compatible than Slang's prebuilt `.so`, so there is no
version-floor conflict: any Linux new enough to run the zig-built `rx_rhi_vk`/sample binaries
(glibc ≥ 2.2.5) is trivially new enough to also satisfy Slang's real floor (glibc ≥ 2.17,
released 2013) — Steam Deck's SteamOS glibc is far newer than either. This is a looser
constraint than Phase 1's brief implied by picking the `-glibc-2.27` archive; that choice is
still fine (it's not the binding constraint either way), just not itself the reason
compatibility holds.
[verified: local `objdump -T`/`readelf -d` measurements against the same downloaded archive;
zig floor verified: local `zig cc` compile + `objdump -T` against `toolchain/zig/zig` (v0.16.0)
already vendored in this repo at `/media/ywadi/second/renderer_x/toolchain/zig/zig`]

**ABI/API stability across releases:** Slang's own C++ interfaces are deliberately COM-style —
per Slang's docs, *"Many parts of the Slang C++ API use interfaces that follow the design of
COM... The `ISlangUnknown` interface is equivalent to (and binary-compatible with) the
standard COM `IUnknown`"* while depending on none of COM's Windows runtime machinery (a
"COM-lite" API) — this is exactly why an MSVC-built `slang-compiler.dll` can be safely linked
from a zig/mingw(LLD)-built executable: pure vtable interfaces + `extern "C"` factory
functions cross the compiler-ABI boundary cleanly (no STL types, no C++ exceptions crossing
it), and LLD's COFF linker reads standard `.lib` import libraries regardless of whether
MSVC's `lib.exe` or `llvm-lib` produced them — worth an explicit empirical check in the Task
4/5 CMake integration, but there is no structural reason it should fail.
Release cadence is fast and **not semver-stable**: recent tags observed via the GitHub API —
`v2026.14.1` (2026-07-30), `v2026.14` (2026-07-24), `v2026.12.0.1` (2026-07-16), `v2026.13.1`
(2026-07-13), `v2026.13` (2026-07-08), `v2026.12.2` (2026-07-01), `v2026.12.1` (2026-06-30),
`v2026.12` (2026-06-25), `v2026.11` (2026-06-15) — multiple releases per month, non-monotonic
patch suffixes (`.0.1`, `.1`, `.2`). Slang has previously made **breaking packaging changes**
between releases: as of `v2024.1.27`, binary layout changed — "for Linux and MacOS, all
libraries are now under `lib/` and all executables are now under `bin/`... for Windows, all
libraries and binaries are now under `bin/`" (which matches exactly what I found in A1: the
Linux/Windows layouts genuinely differ today). **Action for Phase 2:** pin the exact tag
(already done: v2026.14.1) and keep `tools/fetch_slang.cmake`'s "verify the archive's internal
layout, don't assume paths" approach (already flagged in Phase 1's task-4-brief) for any future
version bump — this is a real, historically-demonstrated risk, not a hypothetical one.
[verified: https://github.com/shader-slang/slang release list via
https://api.github.com/repos/shader-slang/slang/releases; COM-lite quote and packaging-change
quote found via https://raw.githubusercontent.com/shader-slang/slang/master/docs/user-guide/compiling.md
and general web search, cross-checked against this session's own archive-layout inspection above]

**Licensing:** primary license is **Apache-2.0 WITH LLVM-exception** (top-level `LICENSE` file
in the release archive, SPDX-tagged). The archive also ships a `LICENSES/` directory covering
bundled-component licenses: `Apache-2.0.txt`, `BSL-1.0.txt` (Boost, likely SPIRV-Tools/glslang
transitive deps), `CC-BY-4.0.txt` (docs), `LicenseRef-UOI-NCSA.txt`, `LLVM-exception.txt`,
`MIT.txt`, `Unlicense.txt`. No GPL/copyleft components observed. Safe for a closed-source
engine DLL to link/redistribute the prebuilt binaries as-is (standard Apache-2.0 attribution
obligations apply: keep the LICENSE/NOTICE when redistributing the binaries).
[verified: local extraction, `LICENSE` + `LICENSES/*.txt` from
slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz]

---

## B. Bindless on Vulkan 1.3

### B1. Which descriptor-indexing features are core-guaranteed in 1.3 vs must be feature-queried

None of the descriptor-indexing bits are *unconditionally* guaranteed just by requiring Vulkan
1.3 — `VkPhysicalDeviceVulkan12Features`/`...Vulkan13Features` promote the structs to core, but
every individual bit remains an optional feature you must query and enable
(`vkGetPhysicalDeviceFeatures2` → check → enable via `pNext` chain at device creation). What
*is* effectively guaranteed for this project's stated baseline is via **Vulkan Roadmap 2022**
(which itself requires Vulkan 1.3): the published Khronos profile `VP_KHR_roadmap_2022`
mandates, among others, all of: `descriptorIndexing`,
`shaderSampledImageArrayNonUniformIndexing`, `shaderStorageBufferArrayNonUniformIndexing`,
`shaderStorageImageArrayNonUniformIndexing`, `shaderUniformTexelBufferArrayNonUniformIndexing`,
`shaderStorageTexelBufferArrayNonUniformIndexing`,
`descriptorBindingSampledImageUpdateAfterBind`, `descriptorBindingStorageImageUpdateAfterBind`,
`descriptorBindingStorageBufferUpdateAfterBind`,
`descriptorBindingUniformTexelBufferUpdateAfterBind`,
`descriptorBindingStorageTexelBufferUpdateAfterBind`, `descriptorBindingUpdateUnusedWhilePending`,
**`descriptorBindingPartiallyBound`**, `descriptorBindingVariableDescriptorCount`, and
**`runtimeDescriptorArray`** — every feature this engine's bindless design needs. This is a
*target-support convention*, not a Vulkan spec guarantee (Roadmap milestones aren't
enforced by the loader/validation layers unless a device explicitly advertises the profile) —
so the RHI must still runtime-query and enable each bit explicitly, but "assume Roadmap-2022-
equivalent hardware" is the correct design assumption for this project's stated floor
(desktop + Steam Deck RDNA2, no mobile).
[verified: https://github.com/KhronosGroup/Vulkan-Profiles/blob/main/profiles/VP_KHR_roadmap_2022.json]

### B2. RADV/RDNA2 (Steam Deck) + mainstream desktop support reality; update-after-bind limits

Pulled the live hardware-capability report for the exact Steam Deck APU
(**AMD Custom GPU 0405** / "Van Gogh", `deviceID 0x163F`, `deviceType INTEGRATED_GPU`, RADV
driver `driverVersion 2.0.270`, `apiVersion 1.3.250`) directly from
`vulkan.gpuinfo.org/displayreport.php?id=31428`:
- `descriptorBindingPartiallyBound = true`, `descriptorBindingVariableDescriptorCount = true`,
  `runtimeDescriptorArray = true`, `descriptorBindingSampledImageUpdateAfterBind = true`,
  `descriptorBindingStorageImageUpdateAfterBind = true`,
  `descriptorBindingStorageBufferUpdateAfterBind = true` — every feature from B1, confirmed
  present on the actual reference floor hardware.
- Update-after-bind limits are effectively **unbounded** on RADV:
  `maxDescriptorSetUpdateAfterBindSamplers = 4294967295`,
  `maxDescriptorSetUpdateAfterBindSampledImages = 4294967295`,
  `maxPerStageDescriptorUpdateAfterBindSampledImages = 4294967295` (i.e. `UINT32_MAX` — RADV
  reports the sentinel "no meaningful limit" value rather than a real cap, consistent with
  AMD's descriptor model being natively bindless-friendly).
- **Pitfall worth flagging:** `shaderSampledImageArrayNonUniformIndexingNative = false` (and
  the storage-buffer/image `...Native` siblings also `false`) on this hardware — nonuniform
  indexing *works* (the base feature is `true`) but is **not free**; RDNA2 needs extra
  re-convergence handling for non-uniform descriptor access, so `NonUniformResourceIndex()`
  should be applied only where indices genuinely vary per-invocation/wave, not reflexively
  everywhere.
- `maxPushConstantsSize = 128` on this device — exactly the Vulkan-mandated minimum, confirming
  the design must budget bindless resource *indices* (not full descriptors) into a tight
  128-byte push-constant block; do not assume more is available cross-platform even though
  many desktop GPUs report more.
- Mainstream desktop reality (self-reported submissions to the same DB, all-platform,
  `VK_EXT_descriptor_indexing`/promoted-core-1.2-equivalent coverage): **Windows 94.25%,
  Linux 92.62%, MacOS 100%** of reporting devices support it — i.e. near-universal on the
  desktop GPU population this project targets.
[verified: https://vulkan.gpuinfo.org/displayreport.php?id=31428 ;
https://vulkan.gpuinfo.org/displayextensiondetail.php?extension=VK_EXT_descriptor_indexing&platform=all]

### B3. Recommended production architecture + Slang's nonuniform-indexing syntax and pitfalls

Verified pattern (matches this project's stated baseline design, and matches current
authoritative guidance from Khronos's own descriptor-indexing sample and vkguide-style
write-ups): one big **global descriptor set** (`set = 0`, say) containing per-resource-type
**unbounded runtime arrays** (`DescriptorTableSlot`/`RuntimeDescriptorArray`-backed bindings —
one binding for sampled images, one for storage buffers, one for samplers, etc., each an
unbounded array), the set layout created with
`VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT` and each binding flagged
`VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` (via
`VkDescriptorSetLayoutBindingFlagsCreateInfo` chained onto
`VkDescriptorSetLayoutCreateInfo::pNext`), and the pool created with
`VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`. Per-draw resource identity is then just a
small set of **indices into those arrays**, passed via push constants (fits comfortably in the
128-byte guaranteed minimum from B2 if kept to a handful of `uint32_t`s — e.g. material index →
one more indirection into a bindless-fetched material buffer, rather than many raw texture
indices per draw).

**Slang's own syntax for non-uniform indexing:** Slang exposes the HLSL intrinsic
`NonUniformResourceIndex()` directly (works for SPIR-V targets); real-world usage has hit
sharp edges — two open upstream issues describe `NonUniformResourceIndex` producing incorrect
SPIR-V (`Incorrect use of 'NonUniform' in SPIR-V`, and a case where it silently fails for
SPIR-V specifically) — so **verify the generated SPIR-V decoration placement with
`spirv-val`/RenderDoc** for any nonuniform-indexed access pattern actually used, don't trust it
blindly on a new Slang version bump. Separately and more significantly: Slang v2026.14.1 ships
a **built-in stdlib module `bindless-storage.slang`** (found in
`lib/slang-standard-module-2026.14.1/slang/bindless-storage.slang` inside the release archive)
defining a `BindlessAddress<T>` type — "pointer-like semantics... wraps a buffer handle and
base index to provide array-like access" over `RWStructuredBuffer<T>.Handle` — plus a general
capability, per Slang's convenience-features docs, that *without* requesting the newer
`spvDescriptorHeapEXT` extension path, "Slang introduces a global array of descriptors and
fetches from it, with the descriptor set ID configurable via the `-bindless-space-index`
option," and additionally accepts HLSL-SM6.6-style `ResourceDescriptorHeap[index]` /
`SamplerDescriptorHeap[index]` syntax directly as input for source compatibility. This means
Slang itself is starting to grow first-class bindless abstractions rather than leaving it
entirely to hand-rolled `set`/`binding` layout — worth evaluating for Phase 2 instead of (or
alongside) a hand-written global-set scheme, though it's new enough (this exact module
appeared in the current pinned version) that I'd treat it as promising-but-unproven rather
than load-bearing on day one.
[verified: local extraction, `lib/slang-standard-module-2026.14.1/slang/bindless-storage.slang`
from slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz; NonUniformResourceIndex SPIR-V bugs:
https://github.com/shader-slang/slang/issues/10525 and
https://github.com/shader-slang/slang/issues/9849 ; descriptor-heap/`-bindless-space-index`
quote: shader-slang convenience-features docs (`shader-slang.org/slang/user-guide/convenience-features`);
update-after-bind flag/pool/binding mechanics:
https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html]

---

## C. Resource management

### C1. VMA 3.4 staging/upload best practice

VMA 3.4.0 (2026-06-05, matches this project's pinned tag) changelog highlights: added
`VmaAllocationCreateInfo::minAlignment`, deprecated `vmaCreateBufferWithAlignment` in its
favor; improved external-memory export/import (`vmaCreateDedicatedBuffer`,
`vmaCreateDedicatedImage`, `vmaAllocateDedicatedMemory` with an extra `pMemoryAllocateNext`
param; `vmaGetMemoryWin32Handle2`); added `VmaVulkanFunctions::vkGetPhysicalDeviceProperties2KHR`
+ `VMA_GET_PHYSICAL_DEVICE_PROPERTIES2` macro (fixes validation-layer warnings from legacy
command usage on Vulkan ≥ 1.1 — relevant since this project pulls in the `*2` function
pointers Phase 1 already flagged as previously-missing); added a `VMA_VERSION` macro; fixed
race conditions in defragmentation and buffer-image granularity handling. **No staging-API
surface changed in 3.4** — the current recommended patterns predate it (introduced ~3.0) and
are documented, verbatim, in VMA's own "Recommended usage patterns" doc:
- **Simple staging buffer** (CPU-fill → GPU transfer): `usage = VMA_MEMORY_USAGE_AUTO`,
  `flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`. "Let the library select the
  optimal memory type, which will always have `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`."
- **ReBAR-aware direct upload** (skip staging entirely when resizable BAR / integrated memory
  makes it a win): same `usage`, `flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
  | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT` (+
  `VMA_ALLOCATION_CREATE_MAPPED_BIT` to get a persistently-mapped pointer back). This "prefers a
  memory type that is both `DEVICE_LOCAL` and `HOST_VISIBLE` (integrated memory or BAR), but if
  no such memory type is available or allocation from it fails... falls back to `DEVICE_LOCAL`
  memory," at which point the app must detect the fallback (check the resulting memory type's
  properties) and issue an explicit staging-buffer transfer instead — i.e. the flag makes VMA
  *try* the fast path but the caller still needs the staging fallback code path for
  non-ReBAR desktop GPUs (which by default expose only 256 MB of BAR unless ReBAR is enabled).
  Given Steam Deck is a unified-memory APU (`deviceType INTEGRATED_GPU` per B2) where this path
  is a strict win, and desktop dGPUs increasingly ship ReBAR-enabled by default, this flag is
  worth using from day one for a small ring-buffer style per-frame upload path, with a true
  staging-buffer + transfer fallback for hardware/BIOS configs without it.
  Steam Deck also being an integrated GPU is directly relevant to this recommendation, not
  just Roadmap 2022 features from B2.
[verified: https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/master/CHANGELOG.md ;
https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/usage_patterns.html]
A **dedicated transfer queue** (`VK_QUEUE_TRANSFER_BIT`-only queue family, if the device
exposes one distinct from graphics) lets uploads overlap with rendering and is the conventional
production pattern for streaming; on unified-memory Steam Deck this matters less (bandwidth is
shared and there's often no separate transfer-capable queue family worth the complexity), so
Phase 2 should support both a single-queue path (simplicity, correct everywhere) and treat a
separate transfer queue as an opportunistic optimization, not a requirement.

### C2. Mipmap generation at upload: blit chain vs compute

Both are legitimate; the honest tradeoff for this engine class: `vkCmdBlitImage`-chain
(level 0→1, insert barrier, 1→2, ...) is the simplest correct implementation and is what
Khronos's own run-time mip-generation sample and most tutorials ship, but (a) it requires the
image format to support `VK_FORMAT_FEATURE_BLIT_DST_BIT`/linear filtering — not guaranteed for
every format (compressed/HDR formats commonly don't), and (b) the level-by-level barrier chain
serializes each step (queue stalls between levels). A cache-aware **compute-shader downsample**
(e.g. NVIDIA's `vk_compute_mipmaps` approach, or AMD's Single Pass Downsampler pattern) can
generate an entire mip chain in one dispatch with better cache behavior; one measured data
point from `vk_compute_mipmaps`' own benchmarking: a 4096×4096 sRGBA8 texture on an RTX 3090
took ~161µs via the blit chain vs ~114µs via compute (~30% faster) — a real but modest win at
that scale. **Recommendation for Phase 2:** start with the blit chain (it's a few dozen lines,
correct, and sufficient for the sample apps' texture sizes) and only reach for a compute
downsampler if profiling on the actual target hardware (Steam Deck bandwidth-constrained iGPU)
shows it matters, or once a format needs custom filtering the blit path can't do (sRGB-correct
box filtering — note libktx's own `ktx create` tool had a real, fixed bug where "sRGB images
being resampled without first decoding to linear," a correctness pitfall equally relevant to a
hand-rolled blit/compute mip generator: linearize before averaging, re-encode to sRGB after).
[verified: https://github.com/nvpro-samples/vk_compute_mipmaps ;
https://docs.vulkan.org/samples/latest/samples/api/texture_mipmap_generation/README.html ;
sRGB mip bug: https://github.com/KhronosGroup/KTX-Software/releases/tag/v4.4.2]

### C3. KTX2/BasisU via libktx — current status and Phase 2 recommendation

Latest **stable** `KTX-Software` release is **v4.4.2** (2025-10-04 — an emergency re-release of
the withdrawn v4.4.1, no functional changes beyond a version-number fix); a **v5.0.0-rc1**
prerelease exists (2026-05-01), not yet a full release, and its release notes explicitly warn
"the legacy tools will be removed in Release 4.5" (superseded language — actual next stable
appears to be heading straight to 5.0). `libktx` supports both KTX1 and KTX2 containers, KTX2
supercompression (zstd/zlib), and Basis Universal encode/transcode
(`ktxTexture2_NeedsTranscoding` / `ktxTexture2_TranscodeBasis`) plus direct Vulkan texture
creation helpers. It is Apache-2.0 (Khronos project), so licensing is not a blocker.
**Recommendation: use `stb_image` (already a natural fit for this project's "prefer ready-made
libraries" rule and its minimal footprint) for Phase 2's sample apps, and explicitly defer full
KTX2/BasisU integration to a later resource-management iteration** — not because libktx is
deficient, but because Phase 2's stated scope (hot-reload demo, bindless textured mesh, basic
streaming demo) doesn't need supercompressed/transcoded GPU-native formats to prove the
architecture; adding libktx's transcoder (which pulls in the Basis Universal codec, a
non-trivial dependency) is better justified once there's a real asset pipeline / real texture
budget pressure (i.e. alongside the asset-import layer, #10 in this project's layer list) than
bundled into the same phase as writing the reflection-driven bindless plumbing. Design the
texture upload path's public surface (raw pixel data + format + dims in, `ITexture` bindless
handle out) so slotting in a libktx-backed loader later doesn't require touching the bindless
descriptor code at all.
[verified: https://github.com/KhronosGroup/KTX-Software/releases (API query, tags v4.4.2/v5.0.0-rc1);
https://github.khronos.org/KTX-Software/libktx/index.html]

---

## D. Samples + deployment

### D1. Sample apps that best demonstrate Phase 2

Honest, few-hundred-lines-each scope, each layering directly on the engine libs already built
(`rx_core`/`rx_platform`/`rx_rhi_vk` + new layer-4/5 code), no scene graph or asset pipeline
required:
1. **`sample_hotreload`** — a fullscreen triangle/quad whose fragment shader color/pattern is
   driven entirely by a `.slang` file on disk; a file-watcher (SDL3 has no built-in watcher —
   simplest honest option is a poll-`stat()`-mtime loop, not inotify, to avoid a new dependency)
   triggers: recreate `ISession` if needed → recompile → new `VkShaderModule` → swap the
   pipeline (or, cleaner given no pipeline caching work is in scope, just recreate the pipeline
   each reload — reloads are rare/interactive, not per-frame). Proves A2/A4 end to end and
   is the cheapest possible demo to get right.
2. **`sample_bindless_mesh`** — load 2-4 hardcoded textured meshes (procedural geometry, e.g.
   a textured cube/sphere + a plane — no mesh importer needed), each texture uploaded through
   the VMA staging path (C1) into the global bindless array (B3), one draw per mesh with a
   push-constant material/texture index, single global descriptor set bound once per frame.
   Proves B + C1 + the reflection-driven pipeline-layout derivation (A3) together, since the
   descriptor set layout and push-constant range should come from Slang reflection on the
   actual shader, not be hand-typed to match it.
3. **`sample_streaming`** — N textures (a few dozen, deliberately more than convenient to keep
   resident) uploaded/evicted from the bindless array at runtime based on a simple
   distance/priority heuristic (doesn't need to be sophisticated — even a fixed round-robin
   "load next, evict oldest" is enough to prove the mechanism), demonstrating that bindless
   indices can be reassigned live via update-after-bind without touching command buffers
   already recorded. This is the one most likely to reveal real synchronization bugs (a texture
   being evicted while still referenced by an in-flight command buffer) — worth budgeting real
   time for, not treating as "just like sample 2 but more textures."
A 4th sample is optional/skippable for Phase 2 scope (e.g. a compute-mipmap-vs-blit
side-by-side toggle demo) — nice-to-have, not essential to prove the layer-4/5 architecture;
recommend cutting it unless time is abundant, per the project's stated preference for honest
scope over padding.

### D2. GitHub Releases distribution gotchas

- **Vulkan loader/ICD is never bundled** by convention and shouldn't be here either: on
  Windows the loader (`vulkan-1.dll`) ships with the GPU driver install (near-universal on any
  machine with a GPU driver from the last decade); on Linux it comes from the distro's
  `vulkan-loader`/`libvulkan1` package plus an ICD registered under `/usr/share/vulkan/icd.d` —
  a sample binary should assume both are present (as any Vulkan game does) and fail with a
  clear message if `vkCreateInstance` can't find a driver, rather than trying to vendor a
  loader (fragile, and the loader's whole job is to find the *system's* driver).
  [verified: https://docs.vulkan.org/guide/latest/loader.html]
- **SDL3 is statically linked** in this project already (`third_party/CMakeLists.txt`:
  `SDL_SHARED=OFF -DSDL_STATIC=ON`), so no `SDL3.dll`/`libSDL3.so` needs to ship — one less
  runtime dependency to get wrong.
- **The Slang shared library situation is the real gotcha, and yes, it must ship next to any
  binary that does runtime Slang compilation** (the hot-reload and bindless-mesh samples, at
  minimum — anything calling into `slang::createGlobalSession`). Per A1/A6: ship
  `libslang-compiler.so.0.2026.14.1` (+ the `libslang.so`/`libslang-compiler.so` symlinks, or
  just resolve by soname) alongside the executable with an RPATH of `$ORIGIN` on Linux, or
  `slang-compiler.dll` next to the `.exe` on Windows (Windows DLL search order checks the
  executable's own directory first, so no PATH/registry tricks needed) — **and also ship the
  plugin `.so`/`.dll`s** (`libslang-glslang-*`, `libslang-glsl-module-*`, `libslang-rt.*`) in
  the same directory even though they're not `DT_NEEDED`/import-time dependencies, since
  they're loaded on demand by certain compile paths and a missing one fails at the point of
  first use, not at load time — easy to miss in testing if the dev machine happens to have them
  cached from a previous SDK install. **Explicitly exclude** `slang-llvm.dll`/equivalent (84 MB
  on Windows) and the deprecated `gfx`/`slang-glslang` GFX-layer artifacts if unused — no need
  to ship the CPU/host JIT backend for a SPIR-V-only sample. Total added redistributable size
  for the compiler + its needed plugins is roughly 25-45 MB depending on platform — noticeable
  for a GitHub Release but far smaller than bundling the full SDK.
- Zig-cross-compiled Windows binaries use the GNU/mingw ABI (per this repo's own
  `windows-cross-zig` toolchain file) — linking against Slang's MSVC-built `slang-compiler.lib`
  should work (see A6's COM-lite ABI argument) but has not been empirically verified in this
  repo yet; flag it as the first thing to smoke-test when Task 5+ actually links Slang, not an
  assumed-safe fact.
- Samples that only ever ship **precompiled** SPIR-V (no runtime Slang calls) don't need any of
  the Slang shared libraries at all — worth keeping that distinction sharp in the sample set
  (D1's `sample_bindless_mesh`/`sample_streaming` could legitimately go either way; only
  `sample_hotreload` strictly requires runtime compilation).

---

## E. Load-bearing findings not explicitly asked for

1. **Slang's own reflection API already gives you a complete, ready-to-consume descriptor-set/
   push-constant model** (`TypeLayoutReflection::getDescriptorSet*` + `BindingType::PushConstant`,
   A3) — there's no need to invent an intermediate shader-metadata format; the layer-4→layer-5
   boundary can be "reflect a linked `IComponentType` → emit `VkDescriptorSetLayoutCreateInfo`
   + `VkPushConstantRange[]` structs directly," which simplifies the spec a lot versus assuming
   a hand-rolled JSON/YAML shader-metadata intermediate.
2. **Slang ships an emerging first-class bindless stdlib module (`bindless-storage.slang`,
   B3) and a `-bindless-space-index` CLI option / `ResourceDescriptorHeap[]` syntax** — this
   wasn't in the ask, but it means the Phase 2 spec should explicitly decide "hand-rolled
   global-set-of-arrays" vs "lean on Slang's native bindless sugar" rather than silently
   defaulting to the former out of not knowing the latter exists; given it's new in the exact
   pinned version, I'd still default to the hand-rolled approach for Phase 2 (more control,
   better understood, verifiable against B2's actual hardware limits) but flag the native path
   as worth a follow-up spike.
3. **The GFX layer bundled with Slang (`slang::gfx`/`gfx.dll`/`libgfx.so`) is explicitly being
   deprecated by its own maintainers in favor of `slang-rhi`** — since this project has its own
   `rx_rhi_vk`, this is purely a "don't accidentally link the wrong target" trap (the CMake
   package exports both `slang::slang` and `slang::gfx`; only the former is wanted), but worth
   stating explicitly in the spec so nobody reaches for the Slang examples' GFX-based sample
   code as a copy-paste source.
4. **RDNA2's `...Native = false` nonuniform-indexing properties (B2)** mean the "just wrap
   every bindless access in `NonUniformResourceIndex()` defensively" instinct has a real,
   measurable cost on the project's own floor hardware — the Phase 2 spec should be explicit
   about *when* nonuniform indexing is required (varying index across a wave/subgroup — e.g.
   different triangles in the same draw picking different materials) vs when it isn't (a single
   per-draw-call push-constant index is uniform across the whole draw and never needs it),
   rather than leaving it to shader-author judgment call by call.
5. **Zig's default (unversioned) Linux target glibc floor is far looser than anything Slang
   requires (A6)** — this means the dependency-cache/toolchain layer from Phase 1 imposes no
   surprise constraint on Phase 2's Slang integration; the two systems compose without any
   glibc-version reconciliation work needed, which is one less cross-cutting risk for the
   Phase 2 spec to carry.
6. **VMA 3.4.0's `vkGetPhysicalDeviceProperties2KHR`/`VMA_GET_PHYSICAL_DEVICE_PROPERTIES2`
   addition (C1)** directly answers a concern Phase 1's own plan document already flagged
   (missing `*2` function pointers for API 1.3 causing VMA assert/fail-to-link) — confirms the
   3.4.0 pin is the right one and this specific historical Phase-1 defect class is fixed
   upstream at this version, not something Phase 2 needs to work around again.
7. **Descriptor-indexing coverage numbers (B2) are self-selected/enthusiast-skewed** (people who
   install a Vulkan capability-viewer tend to have newer hardware) — treat "92-94% coverage" as
   an optimistic upper bound on real-world reach, not a guarantee; the Roadmap-2022 profile
   requirement (B1) plus this project's explicit "desktop + Steam Deck, no mobile" scope is the
   actually load-bearing justification, not the raw percentage.
