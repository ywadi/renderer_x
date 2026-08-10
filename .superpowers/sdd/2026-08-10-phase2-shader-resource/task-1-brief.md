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

