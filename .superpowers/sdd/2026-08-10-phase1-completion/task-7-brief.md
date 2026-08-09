### Task 7: CI matrix, real build-budget check, Wine test fix, sample artifacts

**Files:**
- Create: `.github/workflows/ci.yml`, `tools/check_build_budget.sh`
- Modify: `src/rx_core/tests/log_test.cpp` (one-line CRLF normalization)

**Fixes over the original Task 14:**
1. **Budget check must measure a real incremental build, not a no-op:** `tools/check_build_budget.sh <preset> [budget=60]` does `touch src/rx_core/src/log.cpp` (a leaf .cpp that forces recompile+relink of the dependent chain) BEFORE timing `cmake --build --preset <preset>`; fail if over budget.
2. **zig download integrity:** pin the known sha256 of `zig-x86_64-linux-0.16.0.tar.xz` (compute it from ziglang.org's published checksums — verify, don't invent) and `sha256sum -c` after download in both jobs.
3. **Wine ctest failure:** `log_test.cpp`'s captured-output comparison fails under Wine because Windows spdlog emits `\r\n`. Normalize before comparing (strip trailing `\r` and `\n` from the captured string; compare against `"hello 42"`). This is a 2-line test change that makes the test portable — do it in this task and confirm `rx_core_tests` passes under `wine` locally.
4. **Jobs:**
   - `linux-native` (ubuntu-latest): apt `ninja-build mesa-vulkan-drivers vulkan-tools libvulkan-dev xvfb`; zig install (checksummed); `actions/cache` on `.deps-cache` keyed on `hashFiles('third_party/CMakeLists.txt')`; configure; build; `xvfb-run -a ctest --preset linux-native --output-on-failure` (lavapipe provides the Vulkan device; the SDL window runs under Xvfb — if the GPU-dependent tests genuinely cannot run in this environment, they skip via their existing guards and the job must still prove `sample_01_triangle_headless` ran — investigate rather than accepting silent skips; if lavapipe genuinely can't run it, say so explicitly in the workflow with a comment and a dedicated `ctest -R` allowlist, never a silent pass); budget check.
   - `windows-cross-zig` (ubuntu-latest): apt `ninja-build wine xvfb`; zig (checksummed); `.deps-cache` cache; configure; build; `wine build/windows-cross-zig/tools/toolchain_check/toolchain_check.exe`; `xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|sample' --output-on-failure` (GPU tests excluded — Wine in CI has no Vulkan; rx_core/rx_platform tests run, platform tests hit their skip guards gracefully); budget check.
   - Both jobs: upload the sample binary as a workflow artifact (`sample_01_triangle` / `sample_01_triangle.exe`) with `actions/upload-artifact`.
5. Validate YAML locally, commit, push, `gh run watch` until green — a red first run gets fixed in this task, not deferred.

**Verify:** both CI jobs green on GitHub (for real); local `wine`-run of rx_core_tests passes; budget script measures a real rebuild. Commit clean.

---

