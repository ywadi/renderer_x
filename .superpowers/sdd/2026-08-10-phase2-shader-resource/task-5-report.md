# Task 5 Report: sample_02_hotreload

## Summary

Implemented the full Task 5 scope: `samples/02_hotreload` — a fullscreen
triangle whose fragment shader is compiled at runtime by `rx_shader`'s
`Compiler`, with its `VkPipelineLayout` always built via
`rx::shader::reflect()` + `rx::rhi::PipelineLayoutBuilder::build()` (never a
hand-typed layout, unlike `01_triangle`). Headless ctest gate (embedded
source A → render 2 frames → readback → source B → swap → render → readback
differs) and `--present` mode (on-disk `hotreload.slang`, ~4Hz mtime poll,
recompile-on-change, keep-last-good-on-failure) both implemented, built on
both presets, and manually verified end to end including a real live
reload with screenshots.

Before writing any of this, fast-forwarded the worktree from `0d3387e` to
`ee12102` (bringing in Task 2's landed `reflect()`/`PipelineLayoutBuilder`,
which the as-built context assumed existed but had not yet reached this
worktree's branch point — a zero-conflict fast-forward since this worktree
had no commits of its own yet).

## Findings / discrepancies vs. the as-built context and brief

1. **`rx_shader_deploy_runtime_libs()` is not usable from a sibling
   directory as-is — a real CMake scoping gap, fixed locally, not in
   `src/**`.** The as-built context said "use it for your sample." Calling
   it from `samples/02_hotreload/CMakeLists.txt` produced a build failure:
   ninja invoked `cmake -E copy_if_different <dest-dir>` with no source
   files. Root cause, verified empirically with a minimal standalone CMake
   reproduction: CMake function *names* propagate to a sibling directory
   processed later by the same parent (confirmed with a second minimal
   repro), but an *unqualified variable reference inside the function
   body* resolves through the **calling** scope, not the function's
   *definition* scope. `rx_shader_deploy_runtime_libs()` references
   `RX_SLANG_RUNTIME_LIBS`, a plain (non-CACHE) variable `file(GLOB)`'d in
   `rx_shader/CMakeLists.txt`'s own directory scope — invisible from
   `samples/02_hotreload`'s scope. Fix (entirely within
   `samples/02_hotreload/CMakeLists.txt`): re-glob the identical list
   there (mirroring `rx_shader/CMakeLists.txt`'s patterns verbatim,
   using `RX_SLANG_TARGET_ROOT`, which *is* a CACHE variable and thus
   visible everywhere) immediately before calling the function. Documented
   inline with the empirical finding; **later samples (03/04) linking
   `rx_shader` will need the same few lines** — worth folding into
   `rx_shader_deploy_runtime_libs()` itself in a future pass (e.g. by
   having the function re-glob internally using `RX_SLANG_TARGET_ROOT`
   rather than depending on a pre-globbed variable), but that would touch
   `src/rx_shader/CMakeLists.txt`, outside this task's file scope — flagging
   for the coordinator rather than making the change.
2. **`VkPipelineShaderStageCreateInfo::pName` must be `"main"`, not the
   Slang source-level entry point name.** Naming it `"vsMain"`/`"fsMain"`
   (the names passed to `compileFromSource`/`compileFromFile`) produced
   `VUID-VkPipelineShaderStageCreateInfo-pName-00707` ("No entrypoint found
   named `vsMain`") at `vkCreateGraphicsPipelines`. Slang's SPIR-V backend
   emits each extracted entry point as its own standalone module with
   `OpEntryPoint` always named `"main"`, regardless of the source function
   name — consistent with (and now explained by) why
   `shaders/triangle.vert.slang`/`.frag.slang` name their entry functions
   literally `"main"` in source. Fixed; documented in code.
3. **Reusing one `Compiler`/session across repeated `compileFromFile` calls
   on the same path breaks the second reload.** `compileFromFile` always
   derives the module name from the file's path stem (constant across
   reloads of the same file); Slang's `ISession` appears to cache loaded
   modules by name and rejects new source under a name it has different
   cached source for (`error[E38202]: module already loaded with different
   source`). Found via manual `--present` testing (the headless gate uses
   two *different* module names for its two embedded sources, so it never
   exercised this path). Fixed by having `buildPipelineFromFile` construct
   its own fresh `Compiler`/`ISession` on every call rather than accepting
   one from the caller — cheap (only the process-wide `IGlobalSession`
   pays the real stdlib-load cost, unaffected), and reloads are a rare,
   human-interactive event. This is a real, load-bearing usage-pattern
   finding for `rx_shader::Compiler` that any future file-based hot-reload
   consumer (or `rx_shader`'s own docs) should know about — flagging for
   the coordinator; not a `src/**` change since it's fully addressable at
   the call site.
4. **Embedded ctest colors: white/black, not red/green.** An initial
   red/green version of the headless gate failed: the offscreen target's
   `swapchainFormat()` on this device is BGRA, and red/green are *not*
   invariant under an R↔B channel swap (unlike white/black, which
   `01_triangle`'s own headless gate already relies on for exactly this
   reason). Read back `(0,0,255,255)` for a shader returning pure red —
   correct BGRA bytes, wrong under a naive RGBA-order pixel read. Switched
   to white/black; both are simultaneously sRGB-fixed-points and
   channel-order-invariant.
5. **Task 4 (DeletionQueue) confirmed not landed** at dispatch time, per
   the as-built context's own note — pipeline swap in both modes uses a
   plain `vkDeviceWaitIdle()` before destroying the old pipeline, exactly
   as pre-authorized.

## Implementation

- `samples/02_hotreload/main.cpp`: shared `buildPipelineFromCompileResult`
  core (compile-result → reflect → `PipelineLayoutBuilder::build` →
  `VkShaderModule`s → `VkPipeline`, dynamic rendering, matching
  `01_triangle`'s pipeline state exactly apart from the layout source) with
  two thin wrappers, `buildPipelineFromSource` (shared `Compiler`, used by
  the headless gate's two differently-named embedded sources) and
  `buildPipelineFromFile` (fresh `Compiler` per call, per finding 3 above).
  `ReloadablePipeline` bundles the shader modules, a move-only
  `rx::rhi::PipelineLayoutBundle`, the `VkPipeline`, and the reflected
  push-constant shape (0 or 1 range; more than 1, or any descriptor
  binding, is rejected as out of this sample's scope — logged, not
  silently mismapped). `pushTimeIfDeclared()` pushes an elapsed-seconds
  `float` into that range generically, a no-op when the shader declares
  none.
  - `runHeadless()`: mirrors `01_triangle`'s offscreen-render structure
    (same window/Context/Device/Allocator/CommandContext setup, same
    host-coherence precondition check, same "never write an unacquired
    swapchain image" discipline) — compiles source A, renders 2 frames,
    reads back; compiles source B, `vkDeviceWaitIdle` + swap, renders 2
    frames, reads back; asserts both colors and their difference; checks
    `context->hasValidationErrors()`.
  - `runPresent()`: mirrors `01_triangle`'s `FrameSync` present loop
    exactly, with one addition — once per loop iteration, rate-limited
    internally to ~250ms, checks `hotreload.slang`'s mtime
    (`std::filesystem::last_write_time`, `error_code` overload so a
    momentarily-missing file during an editor's atomic save is not fatal)
    and, on change, recompiles/rebuilds/swaps (or logs+keeps-last-good on
    failure) via a plain `vkDeviceWaitIdle` swap at a point where no
    frame's command buffer is mid-flight (top of the loop, before that
    iteration's fence wait/acquire).
- `samples/02_hotreload/hotreload.slang`: the live-editable on-disk shader
  — fullscreen triangle (oversized clip-space triangle from `SV_VertexID`,
  no vertex buffer) + a `[[vk::push_constant]] ConstantBuffer<PushConstants>
  { float time; }` driving a moving diagonal stripe pattern by default;
  heavily commented as an edit target.
- `samples/02_hotreload/CMakeLists.txt`: re-globs `RX_SLANG_RUNTIME_LIBS`
  locally (finding 1), calls `rx_shader_deploy_runtime_libs()`, and adds a
  `POST_BUILD` `copy_if_different` step deploying `hotreload.slang` next to
  the binary. Registers `sample_02_hotreload_headless` via `add_test`.
- Root `CMakeLists.txt`: one line, `add_subdirectory(samples/02_hotreload)`.
- `samples/README.md`: new `## 02_hotreload` section (what it demonstrates,
  live-editing walkthrough with a concrete edit example, expected output,
  redistribution notes) plus updated Linux/Windows build-and-run command
  blocks and the automated-test-suite section to mention both samples.

## Verification

- **linux-native, full build + ctest:** all targets build clean; `ctest
  --preset linux-native`: **7/7 pass**
  (`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`,
  `rx_shader_tests`, `rx_rhi_vk_tests`, `sample_01_triangle_headless`,
  `sample_02_hotreload_headless`). Direct run of
  `sample_02_hotreload` (no ctest wrapper) confirmed the only validation
  output present across the whole run is this project's two pre-existing,
  already-documented false-positive guards (`VK_KHR_portability_enumeration`
  and the Slang `SourceLanguage` operand warning) — zero unexpected
  validation errors.
- **windows-cross-zig, full configure + build + ctest:** clean configure,
  all 53 targets build including `sample_02_hotreload.exe` with all four
  Slang DLLs (`slang-compiler.dll`, `slang-glslang.dll`,
  `slang-glsl-module.dll`, `slang-rt.dll`) deployed next to it; `ctest
  --preset windows-cross-zig`: **7/7 pass**, including
  `sample_02_hotreload_headless.exe` actually executed under Wine on this
  machine with a real Vulkan device (this preset's ctest is not
  GPU-excluded in this repo's current configuration).
- **`--present` mode, manually verified on this machine (screenshots
  taken, DISPLAY=:1, ImageMagick `import -window`):**
  1. Launched `--present`; log confirmed the resolved on-disk shader path
     and window open; screenshot showed the expected animated diagonal
     stripe pattern.
  2. Edited the *deployed* `hotreload.slang` (next to the binary, not the
     source-tree copy) to a yellow/black checkerboard, no time dependency;
     log showed `changed on disk, recompiling...` → `reload succeeded,
     pipeline swapped`; screenshot confirmed the checkerboard rendering.
  3. Edited it again to an intentionally broken shader (undefined
     identifier); log showed Slang's real diagnostic (source line +
     caret) → `reload failed ... keeping the last-good pipeline`;
     confirmed via `ps` the process was still alive and a screenshot still
     showed the *previous* (checkerboard) pattern, unchanged — no crash,
     no blank window.
  4. Fixed the shader (solid blue); log showed a successful reload.
  5. Sent `SIGTERM`; log showed `--present: window closed cleanly`, process
     exited, `context->hasValidationErrors()` clean.
  - This manual pass is also what surfaced findings 2 and 3 above — the
    headless ctest gate alone would not have caught either (it never
    reloads the *same* module name twice, and never creates a real
    `VkPipelineShaderStageCreateInfo` from a custom-named entry point
    without this fix already in place, since both were fixed before the
    headless gate was written against the corrected code).
- **`-Wall -Wextra` recompile of `main.cpp`** (standalone invocation of the
  exact `zig-cxx-linux` command ninja uses, with those flags added):
  zero warnings.
- `git status`: clean after commit; only the three worktree-setup symlinks
  (`toolchain`, `.deps-cache`, `third_party/slang-prebuilt`) remain
  untracked, as instructed.

## Deviations from brief / spec

None at the Fixed-Decisions level. Two adaptations, both scoped entirely
within `samples/02_hotreload/**` (no `src/**` changes):
1. `samples/02_hotreload/CMakeLists.txt` re-globs `RX_SLANG_RUNTIME_LIBS`
   locally before calling `rx_shader_deploy_runtime_libs()` (finding 1).
2. `buildPipelineFromFile` constructs its own `Compiler` per call instead
   of reusing a caller-supplied one (finding 3) — the brief's plan text
   ("`Compiler::compileFromFile` → on success build new pipeline...")
   didn't specify session lifetime; this is the minimal fix keeping that
   contract while avoiding the module-name collision.

## Concerns for the coordinator

1. **`rx_shader_deploy_runtime_libs()`'s `RX_SLANG_RUNTIME_LIBS` scoping
   gap (finding 1) will recur verbatim for Task 6/7's samples** (both link
   `rx_shader` per the plan's Components table). Worth a small follow-up in
   `src/rx_shader/CMakeLists.txt` to have the function re-glob internally
   from `RX_SLANG_TARGET_ROOT` (a CACHE variable, visible everywhere)
   instead of depending on a pre-globbed caller-scope variable — would
   remove the need for every future sample's `CMakeLists.txt` to repeat
   this glob. Not made here since it's a `src/**` file outside this task's
   scope.
2. **`rx_shader::Compiler`'s "session reuse" contract and same-path
   `compileFromFile` reloads are in tension (finding 3).**
   `compiler.h`'s doc comment says session reuse is "what a hot-reload loop
   relies on," but reusing one session to reload the *same file path*
   repeatedly fails outright. Worth a documentation note on `Compiler`
   itself (or a future `unloadModule`/`invalidate` API) so the next
   consumer doesn't rediscover this the hard way — this sample's fix
   (fresh `Compiler` per file-reload) works and is cheap, but it's a
   sample-level workaround, not a documented contract.
3. **`pName` must always be `"main"`** for any consumer of
   `rx_shader`-compiled SPIR-V building a real `VkPipelineShaderStageCreateInfo`
   (finding 2) — not currently documented anywhere in `rx_shader`'s headers;
   Task 6/7 will hit the same thing when they build real pipelines from
   compiled/reflected shaders. Worth a one-line doc note on `SpirvBlob` or
   `CompileResult`.
4. Manual `--present` verification was performed by this agent (screenshots
   via `ImageMagick import` against a real `DISPLAY=:1` X session on this
   machine, real Vulkan device), not a human — flagging in case the
   coordinator wants an additional human pass before release, per this
   repo's `MANUAL_VERIFICATION.md` convention (that file itself was not
   updated, since it's outside this task's file scope — 01_triangle is its
   only current section).

## Files created

- `samples/02_hotreload/main.cpp`
- `samples/02_hotreload/hotreload.slang`
- `samples/02_hotreload/CMakeLists.txt`

## Files modified

- `CMakeLists.txt` (+1 line: `add_subdirectory(samples/02_hotreload)`)
- `samples/README.md` (new `## 02_hotreload` section; updated build/run/
  ctest instructions)

## Commit

`deb8855d5d9d095a57e6a7a443787f6fab47f1d5` — "Add samples/02_hotreload:
runtime Slang compilation + live shader reload" (worktree branch
`worktree-agent-a381281ec33251156`, fast-forwarded from `0d3387e` to
`ee12102` before this task's own commit to pick up Task 2).

## Fix note (post-review)

Review came back Approved with one Important finding: `ReloadablePipeline`
stored `pushConstantSize`/`pushConstantStages` but never
`pushRanges[0].offset`, and `pushTimeIfDeclared` called `vkCmdPushConstants`
with a hardcoded `offset=0` — correct today only because
`hotreload.slang`'s single `[[vk::push_constant]]` global happens to
reflect at offset 0, with nothing enforcing that staying true across a
future shader edit.

Fixed by plumbing the real offset through (chosen over rejecting a nonzero
offset at build time, matching this sample's existing pattern for >1 push
range/any descriptor binding): added `ReloadablePipeline::
pushConstantOffset`, set alongside `pushConstantSize`/`pushConstantStages`
from `layoutInfo->pushRanges[0].offset` in
`buildPipelineFromCompileResult`, reset in `destroyReloadablePipeline`, and
passed as the real `offset` argument to `vkCmdPushConstants` in
`pushTimeIfDeclared` instead of the previous literal `0`. Rationale
(also documented at the site): a live-editable shader is the entire point
of this sample, so honoring whatever `reflect()` actually reports is the
more correct failure mode than arbitrarily rejecting a valid Slang/Vulkan
construct the host code had merely taken a shortcut around.

Verified the value actually flows, not just compiles: a temporary
`RX_LOG_INFO` at the assignment site (removed before the fix commit) showed
`pushConstantOffset=0 size=4` when building `hotreload.slang`'s real
pipeline via `--present` mode (the embedded ctest sources declare no push
constant at all, so the headless gate alone doesn't exercise this
assignment). Re-ran after the fix:
- `cmake --build --preset linux-native` clean; `ctest --preset
  linux-native`: **7/7 pass** (unchanged from before the fix).
- `cmake --build --preset windows-cross-zig` clean; `ctest --preset
  windows-cross-zig`: **7/7 pass** under Wine (unchanged).
- Standalone `-Wall -Wextra` recompile of `main.cpp`: zero warnings.
- `git status` clean after commit; `git log -1 --format='%B'` confirmed no
  AI attribution.

New commit: `857fb45b98f0920fd174eb5c800bb8b98849b026` — "Fix Task 5 review
finding: push-constant offset was hardcoded to 0" (same worktree branch,
on top of `deb8855d5d9d095a57e6a7a443787f6fab47f1d5`). Not pushed, per
instructions.
