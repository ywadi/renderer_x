### Task 5.5 (ad-hoc): Fix windows-cross-zig missing RC compiler

**Discovered during:** Task 5's review, via filesystem forensics on `build/windows-cross-zig/` (no `build.ninja` present; `_deps-build/` contains only `spdlog`, meaning configure died inside spdlog's CMake subbuild before reaching anything else). Independently confirmed the `windows-cross-zig.cmake` toolchain file sets `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` but never `CMAKE_RC_COMPILER`, and CMake's own auto-detection resolves to a nonexistent `zig-windres` binary on this target.

**Why this matters:** Every dependency added via `rx_add_cached_dependency` in `third_party/CMakeLists.txt` applies to both presets unconditionally (there's no preset-conditional logic in `cmake/DepCache.cmake` or `third_party/CMakeLists.txt`). Since spdlog (Task 3) already breaks a clean `windows-cross-zig` configure, this will get worse as Tasks 6-9 add vk-bootstrap, and it will hard-block Task 14's CI matrix, which explicitly builds+tests `windows-cross-zig` end to end.

**Files likely involved:**
- Modify: `cmake/toolchains/windows-cross-zig.cmake` (set `CMAKE_RC_COMPILER` to something zig can actually execute)
- Possibly create: a new wrapper script under `cmake/zig-wrappers/` (e.g. `zig-rc-windows`), mirroring the existing `zig-cc-windows`/`zig-cxx-windows` pattern — zig has a `zig rc` subcommand that can act as a windres-compatible resource compiler. Verify this actually works before committing to it; if `zig rc` doesn't behave as a drop-in `windres`/`CMAKE_RC_COMPILER`, find the actual correct fix (e.g., pointing `CMAKE_RC_COMPILER` at `llvm-rc` if zig bundles one, or another approach) — investigate rather than guessing.

**Investigate first:** reproduce the failure yourself before fixing:
```bash
rm -rf build/windows-cross-zig
cmake --preset windows-cross-zig
```
Read the actual CMake error output carefully — confirm it's really the RC compiler (don't assume the diagnosis above is complete; verify it against the real error text) and understand exactly what triggers RC-compiler detection here (likely CMake's generic Windows-GNU platform module auto-probing for one once `CMAKE_SYSTEM_NAME=Windows` + a GNU-ABI-like compiler ID is set, even though nothing in this project currently compiles a `.rc` file).

**Fix requirement:** `windows-cross-zig` must configure AND build cleanly from a totally clean state (`rm -rf build/windows-cross-zig .deps-cache` — note: nuking `.deps-cache` also forces a fresh spdlog cross-build for this triple, which is the actual failure condition; this is a full, real test, not a shortcut) — through to a successful `cmake --build --preset windows-cross-zig` of the current target set (`toolchain_check`, `dep_cache_smoketest`, `rx_core_tests`, `rx_platform_tests` if they cross-compile cleanly — note `rx_platform_tests`/`rx_core_tests` may or may not currently be wired for Windows; building whatever the current `CMakeLists.txt` produces for this preset is the bar, don't add new targets).

**Must not break:** re-verify after your fix that `linux-native` still configures+builds+tests cleanly from scratch (`rm -rf build/linux-native .deps-cache && cmake --preset linux-native && cmake --build --preset linux-native && ctest --preset linux-native`) — this fix must be purely additive to the Windows toolchain file, not something that risks the already-approved Linux path.

**Report:** what the actual root cause was (verified, not assumed), the fix, and the two clean-build verification transcripts (windows-cross-zig succeeding, linux-native still succeeding) — both from a fully clean state, not an incremental rebuild.
