# Task 5 report: rx_material core (modules, pass signatures, variant cache)

**Worktree:** `/media/ywadi/second/renderer_x/.claude/worktrees/agent-a12b9aa263ca60061`
**Branch:** `worktree-agent-a12b9aa263ca60061`
**Commit:** `51f731c11038de994541cf1373ab49034ff14bb1` — "feat: add rx_material core with pass-signature-keyed pipeline cache"

Note: this worktree was branched from an older point on `main` (before rx_graph's
Tasks 1-3 landed). Before any build, I fast-forwarded the branch to `main`'s tip
(`git merge --ff-only main`, commit `5c4aa23`) to pick up rx_graph — a clean
fast-forward since the worktree had zero commits of its own at that point, verified
via `git merge-base --is-ancestor HEAD main` first. All Task 5 work is on top of
that fast-forward, in the one commit above.

## What was built

- `src/rx_graph/include/rx_graph/pass_signature.h` (new): `rx::graph::PassSignature`
  — color formats (8-slot, `VK_FORMAT_UNDEFINED`-padded array) + count, depth format,
  sample count, `operator==`, and an inline FNV-1a-64 `hash()` over the fields
  (not a raw memory scan, so equal signatures always hash equal regardless of
  padding).
- `src/rx_graph/include/rx_graph/executor.h` / `executor.cpp` (modified, additive
  only): `PassContext::passSignature() const`, populated by `Executor::execute()`
  from the same `colorPhysIdx`/`depthPhysIdx` classification it already computes
  for `vkCmdBeginRendering`. Added to `executor.h` (where `PassContext`'s real
  definition lives), not `pass.h` — `pass.h` only forward-declares `class
  PassContext;`; the brief's abbreviated file list names `pass.h`, but
  `executor.h` already carried a forward-looking comment ("Task 5 adds
  passSignature() here") anticipating this, which is what I followed.
- `shaders/material/material.slang` (new): `IMaterialShader` interface +
  `MaterialVertex` struct, both `public` (required for cross-module visibility
  under Slang's access-control rules).
- `shaders/material/forward_entry.slang` (new, not in the brief's literal file
  list but required by its own "Shader-side contract" prose): the shared
  vertex+fragment entry-point module, generic over materials via Slang's
  `extern`/`export` link-time-type mechanism (see below). Fixed vertex-input
  layout: `position`/`normal`/`uv` (float3/float3/float2, interleaved,
  locations 0/1/2) — Phase 2 never fixed one canonical host-side vertex struct
  across samples, so this is a new, documented convention scoped to material
  pipelines.
- `src/rx_material/`: `CMakeLists.txt`, `include/rx_material/material_system.h`,
  `material_system.cpp`, `tests/{CMakeLists.txt,doctest_main.cpp,
  test_material_system.cpp,data/{test_unlit,test_solid,test_bad_syntax}.slang}`.
  `MaterialSystem` drives `slang::ISession`/`IModule`/`IComponentType` directly
  (not through `rx_shader::Compiler`, whose `compileFromSource`/`compileFromFile`
  compile exactly one module + its own entry points — too narrow for composing
  a material module against a *second*, separately-loaded shared entry-point
  module). It follows `Compiler`'s established idioms throughout (session/
  target-desc construction, diagnostics-blob capture on every step regardless
  of success, the same-module-name-per-session caveat) rather than inventing a
  different shape.
- Root `CMakeLists.txt`: `add_subdirectory(src/rx_material)` after `rx_graph`.
- `.github/workflows/ci.yml`: added `rx_material_gpu` to the windows-cross-zig
  job's `ctest -E` exclusion regex (Wine has no real Vulkan device; every
  `rx_material_gpu_tests` case builds a real `VkPipeline`). Not in the brief's
  literal file list, but the brief's binding constraint explicitly calls for
  "the windows-cross exclusion naming convention like rx_graph_gpu_tests," and
  the existing exclusion mechanism lives entirely in this file (a regex on
  `ctest -E`, not a CMake property) — mirrored the existing pattern.

## Slang specialization mechanism used (with citation)

Static link-time specialization via `extern`/`export` type aliasing —
Slang's own documented recommended path (*not* `IComponentType::specialize()`
with explicit `TypeReflection*` arguments, and *not* existential/`anyValueSize`
dynamic dispatch):

- `forward_entry.slang` declares `extern struct MaterialImpl : IMaterialShader;`
  and uses it by name in `fragmentMain`.
- Each material module (e.g. `test_unlit.slang`) declares
  `export struct MaterialImpl : IMaterialShader = Unlit;`.
- Host side: `session->createCompositeComponentType({forwardEntryModule,
  vertexEntryPoint, fragmentEntryPoint, materialModule}, ...)` then
  `composite->link(...)`, then `linked->getEntryPointCode(0/1, 0, ...)`.

Citation: [Link-time Specialization and Module Precompilation — Slang user
guide](https://shader-slang.org/slang/user-guide/link-time-specialization)
("Link-time Types" section — the `ISampler`/`Sampler`/`FooSampler` worked
example is the exact shape this follows, `extern struct X : IFace;` /
`export struct X : IFace = Concrete;`), source at
`github.com/shader-slang/slang/blob/master/docs/user-guide/10-link-time-specialization.md`.

I verified this end-to-end against the project's actual shipped Slang
v2026.14.1 build *before* writing any production code: a standalone C++
probe (session → load `material.slang`/`forward_entry.slang`/a test material
→ compose 4 parts → link → `getEntryPointCode`) compiled, linked, and
produced correct SPIR-V (checked via `spirv-dis`) — this is the toolchain's
real, supported path for the shape this task needs, not just documentation.

## Reflection finding (empirically verified, not assumed)

`rx_shader::reflect()` deliberately skips `ParameterBlock<T>` globals
(documented in `reflection.h`). A material's `ParameterBlock<TParams> gParams`
reports Slang category `SubElementRegisterSpace` (not `DescriptorTableSlot`,
the only category `reflect()` handles). For that category, on this shipped
build, `VariableLayoutReflection::getBindingIndex()` — not `getBindingSpace()`,
which reports 0 regardless — returns the parameter's *real* descriptor set
number. Verified three times against `spirv-dis` on real emitted SPIR-V at
three different explicit `[[vk::binding(0, N)]]` values (N=1, N=3, N=1 again
on the final production files): `getBindingIndex()` matched the real
`DescriptorSet` decoration exactly every time. `MaterialSystem` uses this
directly (reads the real set via `getBindingIndex()`, validates it equals the
engine's fixed convention of set 1, and hardcodes binding 0 within that set —
Slang's own documented behavior for a resource-field-free `ParameterBlock`,
not a workaround). Materials declaring anything else at global scope besides
exactly one such parameter block are rejected with a clear error — a
deliberate Task 5 scope boundary (D8 routes textures through `gParams`'s own
bindless-table indices, never a second global), not an oversight; a flat
set-0-binding reflection path is real, buildable future work but has zero
test coverage today, so it isn't shipped speculatively.

## Test results

```
ctest --preset linux-native -R rx_material --output-on-failure
    Start 8: rx_material_gpu_tests
1/1 Test #8: rx_material_gpu_tests ............   Passed    2.94 sec
100% tests passed, 0 tests failed out of 1
```

6 test cases / 64 assertions, all passing:
load-reflect, cache-hit (compile-counter flat across both `getPipeline()`
calls), cache-key-pass (different depth format → different `VkPipeline`),
cache-key-material (two modules, same signature → different `VkPipeline`),
bad-module (throws `std::runtime_error` containing Slang's `error[...]`
diagnostic text), pipeline-cache-persists (destroy/recreate against the same
path — file exists, non-empty, load path logged at info level, second
`create()` succeeds).

Full linux-native suite (12 targets, including this one) and windows-cross-zig
(9 non-excluded targets under Wine, `rx_material_gpu_tests` correctly skipped)
both pass. Both presets build clean, from scratch, with no errors:
`cmake --build --preset linux-native` and `cmake --build --preset
windows-cross-zig` each complete their full target set (53/53).

## Validation evidence

`rx_material_gpu_tests` registered with `--validate` (every fixture builds
`rx::rhi::Context` with `enableValidation=true` unconditionally, matching
every other GPU-backed test binary's own convention); every `TEST_CASE` asserts
`CHECK_FALSE(context.hasValidationErrors())`. First run surfaced one real
(non-false-positive) message: "vertex shader writes to output location N
which is not consumed by fragment shader" (a legal-but-flagged interface
mismatch — `forward_entry.slang`'s shared vertex stage always writes
`worldPos`/`normal`/`uv`, but a flat/trivial material's fragment stage that
never reads one gets that read dead-code-eliminated per-entry-point, and
Slang's SPIR-V backend does not perform joint cross-stage trimming). Fixed at
the shader level (not in `rx_rhi_vk/context.cpp`'s shared false-positive
filter, which is out of this task's scope and reserved for genuine layer
bugs): `test_unlit.slang`/`test_solid.slang` now genuinely read every
`MaterialVertex` field via a non-constant-foldable `* 1e-4` term, verified via
`spirv-dis` to restore the matching Input interface before re-running the
real GPU test. Final run: zero validation messages across all 6 test cases,
full linux-native suite, and the windows-cross-zig non-GPU subset under Wine.

## Files

- `src/rx_graph/include/rx_graph/pass_signature.h` (new)
- `src/rx_graph/include/rx_graph/executor.h` (modified)
- `src/rx_graph/executor.cpp` (modified)
- `shaders/material/material.slang` (new)
- `shaders/material/forward_entry.slang` (new)
- `src/rx_material/CMakeLists.txt` (new)
- `src/rx_material/include/rx_material/material_system.h` (new)
- `src/rx_material/material_system.cpp` (new)
- `src/rx_material/tests/CMakeLists.txt` (new)
- `src/rx_material/tests/doctest_main.cpp` (new)
- `src/rx_material/tests/test_material_system.cpp` (new)
- `src/rx_material/tests/data/test_unlit.slang` (new)
- `src/rx_material/tests/data/test_solid.slang` (new)
- `src/rx_material/tests/data/test_bad_syntax.slang` (new)
- `CMakeLists.txt` (modified, root)
- `.github/workflows/ci.yml` (modified)

## Concerns

1. **`RX_MATERIAL_SHADER_DIR` is a compile-time-baked absolute path**
   (`${CMAKE_SOURCE_DIR}/shaders/material`), matching `shaders/CMakeLists.txt`'s
   own existing precedent for `shader_spirv_test` (`RX_TRIANGLE_VERT_SPV`
   etc.). Correct and sufficient for this task (core library + its own
   build-tree-run tests, exactly like `rx_shader_tests`/`rx_graph_tests`), but
   not redistribution-safe — a future sample shipping a packaged binary needs
   to revisit this (`MaterialSystem::create()`'s signature has no
   directory parameter to add one without an interface change). Flagged
   explicitly in `material_system.h`'s own comment, not hidden.
2. **Fresh `slang::IGlobalSession` per `MaterialSystem`**, not
   `rx_shader::Compiler`'s process-wide shared one (which is private to
   `compiler.cpp`, not exported, and rx_shader is outside this task's Modify
   list). Pays Slang's stdlib-load cost again if both are alive at once in
   the same process — acceptable for a class built once per renderer
   lifetime, but worth knowing if `rx_shader::Compiler` and `MaterialSystem`
   both end up live in the same sample process later.
3. **No flat set-0 resource reflection path** for a material that directly
   declares its own bindless-shaped globals (see "Reflection finding" above)
   — rejected with a clear error today. D8's own design (textures via
   `gParams` bindless indices) means this may never be needed, but it is a
   real, not-yet-exercised gap if a future material wants it.
4. **Hot reload (D9) is out of scope** — no reload test exists in this
   task's list, and `loadMaterial()` inherits `rx_shader::Compiler`'s
   documented same-module-name-per-session caveat verbatim (reloading a
   changed file through the same `MaterialSystem` fails); a future reload
   path needs a fresh `MaterialSystem` (fresh session) per reload, exactly
   like `samples/02_hotreload` already established for `Compiler`.
5. Rasterization/blend/depth-stencil fixed-function state in
   `getPipeline()` (opaque blend, back-face cull, CCW front face) is a
   reasonable default with no test coverage of actual rendering correctness
   (Task 5 has no pixel-readback gate) — first real exercise will be a later
   sample.
