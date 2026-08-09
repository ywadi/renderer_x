# Task 4 report: Slang prebuilt fetch + triangle shaders

Worktree: `/media/ywadi/second/renderer_x/.claude/worktrees/agent-ae1008d3da87e4133`
Branch: `worktree-agent-ae1008d3da87e4133`
Commit: `ffa30be` — "Add prebuilt Slang fetch and triangle demo shader compilation"

**Note on this file's location:** the brief asked for this report at the
main-repo path
`/media/ywadi/second/renderer_x/.superpowers/sdd/2026-08-10-phase1-completion/task-4-report.md`.
This session's sandbox enforces worktree isolation and refused a direct
write there ("Edit the worktree copy of this file instead of the
shared-checkout path"), so this report is written to the worktree's own
copy of the same relative path instead. The coordinator will need to copy
it into the main repo.

## What was built

- `tools/fetch_slang.cmake` — fetches the prebuilt Slang 2026.14.1 release
  archive for the **host** (guarded on `CMAKE_HOST_SYSTEM_NAME STREQUAL
  "Linux"`, not `CMAKE_SYSTEM_NAME`, so it fetches the Linux archive under
  both `linux-native` and `windows-cross-zig`). Downloads via
  `file(DOWNLOAD ... STATUS)` with `FATAL_ERROR` naming the URL on failure,
  extracts via `file(ARCHIVE_EXTRACT)` into
  `third_party/slang-prebuilt/linux-x86_64/`, writes a `.rx-fetched` marker
  for idempotency, verifies `bin/slangc` exists post-extraction (fails loudly
  with the inspected path otherwise), and sets `RX_SLANGC` as a `FILEPATH`
  cache variable.
- `tools/fetch_slang_test.sh` — standalone verification script; asserts
  `slangc -v` reports `2026.14.1`.
- `shaders/triangle.vert.slang` / `shaders/triangle.frag.slang` — triangle
  demo shaders per brief (hardcoded NDC positions selected by
  `SV_VertexID`, solid white, `[shader("vertex")]`/`[shader("fragment")]`
  entry points named `main`).
- `shaders/CMakeLists.txt` — `add_custom_command` per shader invoking
  `slangc` with `DEPENDS` on the `.slang` source (so edits trigger
  regeneration, verified — see below), `add_custom_target(triangle_shaders
  ALL ...)`, cache variables `RX_TRIANGLE_VERT_SPV` / `RX_TRIANGLE_FRAG_SPV`
  (`CACHE INTERNAL`, absolute paths), and the `shader_spirv_test` executable
  + `add_test`.
- `shaders/tests/spirv_validity_test.cpp` — doctest executable reading both
  `.spv` files and checking the first 4 bytes compose to the SPIR-V magic
  number `0x07230203`, built explicitly byte-by-byte (not a reinterpret of
  a `uint32_t`) so it's correct regardless of host endianness. Paths come in
  via `target_compile_definitions` (`RX_TRIANGLE_VERT_SPV`/`_FRAG_SPV` as
  string-literal macros).
- Root `CMakeLists.txt`: added `include(tools/fetch_slang.cmake)` and
  `add_subdirectory(shaders)` right after `add_subdirectory(third_party)`
  (needs `doctest::doctest`, defined there) and before `src/rx_core`.

## Real archive layout (verified, not assumed)

Downloaded and extracted
`slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz` directly and inspected the
tree before writing any CMake logic. Findings:

- **No top-level wrapper directory** — `bin/`, `include/`, `lib/`, `share/`,
  `LICENSE`, `LICENSES/`, `README.md` sit right at the archive root.
- `bin/slangc` is present as expected (so the brief's `bin/slangc` guess was
  correct here, but only confirmed after inspection, not assumed).
- `slangc`'s ELF `RUNPATH` is `$ORIGIN/../lib:$ORIGIN` — it resolves its own
  shared libs (`libslang.so` etc.) relative to its own location. No
  `LD_LIBRARY_PATH` setup needed by the custom-command invocation or the
  verification script.
- `slangc -v` prints `2026.14.1` **to stderr, not stdout**. This bit
  `tools/fetch_slang_test.sh` on first run (captured only stdout → empty
  string, comparison failed even though the version was correct) — fixed by
  capturing `2>&1` in the command substitution. Flagged explicitly since
  it's an easy trap for anyone touching this script later.

## Actual slangc flags used

The brief's proposed invocation worked exactly as given — verified directly
against `slangc -h` and by running it against both real shader files before
wiring it into CMake:

```
slangc <src> -target spirv -profile sm_6_0 -entry main -o <out>
```

No flag drift found against this pinned 2026.14.1 release; no adaptation
was needed. (`-target`, `-profile`, `-entry`, `-o` all confirmed present and
behaving as documented in `slangc -h` output.)

## Verification performed

All run from the worktree, in order, using the symlinked `toolchain/` and
`.deps-cache/`:

1. `rm -rf build && cmake --preset linux-native` — configured clean; log
   shows `[fetch_slang] Fetching Slang 2026.14.1 prebuilt (linux-x86_64)
   from https://github.com/shader-slang/slang/releases/download/v2026.14.1/slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz ...`
   then `Configuring done` / `Generating done`.
2. `cmake --build --preset linux-native` — 28/28 build steps succeeded,
   including `[1/28] slangc: triangle.frag.slang -> triangle.frag.spv` and
   `[2/28] slangc: ... .vert.spv`.
3. `ctest --preset linux-native -R shader_spirv_test` — `1/1 Test #1:
   shader_spirv_test ... Passed`.
4. `ctest --preset linux-native` (full suite) — `100% tests passed, 0 tests
   failed out of 4` (`shader_spirv_test`, `rx_core_tests`,
   `rx_platform_tests`, `rx_rhi_vk_tests`).
5. `./tools/fetch_slang_test.sh` — `fetch_slang_test: OK - slangc -v reports
   2026.14.1 (.../third_party/slang-prebuilt/linux-x86_64/bin/slangc)`.
6. Idempotency: recorded the `.rx-fetched` marker mtime, ran `cmake --preset
   linux-native` a second time — log shows `[fetch_slang] Slang 2026.14.1
   prebuilt already present at ... (marker found) - skipping download`, and
   the marker mtime was unchanged (confirmed via `stat`). Rebuild afterward
   was `ninja: no work to do.` — the `.spv` outputs' mtimes were also
   unchanged.
7. Regeneration-on-change: `touch shaders/triangle.vert.slang` then
   rebuilt — only `triangle.vert.spv` was recompiled (`[1/1] slangc:
   triangle.vert.slang -> triangle.vert.spv`); `triangle.frag.spv`'s mtime
   was untouched. Confirms the `DEPENDS` wiring is per-shader, not a blanket
   rebuild-everything target.
8. `rm -rf build/windows-cross-zig && cmake --preset windows-cross-zig` —
   configured clean; log confirms it reused the *same* Linux slangc fetch
   (`already present ... skipping download` — no attempt to fetch a Windows
   archive), proving the `CMAKE_HOST_SYSTEM_NAME` guard works as intended
   under the cross-compiling preset.
9. `cmake --build --preset windows-cross-zig` — 28/28 build steps succeeded,
   including `slangc` compiling the same two shaders (host-side) and
   linking `shaders/shader_spirv_test.exe` (cross-compiled Windows PE test
   binary — not run here, since Wine wasn't invoked in this pass, but it
   linked cleanly against the cross-compiled doctest static lib).
10. `git status` before and after `git add` — confirmed only the intended
    files (`CMakeLists.txt`, `shaders/**`, `tools/fetch_slang.cmake`,
    `tools/fetch_slang_test.sh`) were staged; `toolchain` and `.deps-cache`
    symlinks stayed untracked and were excluded from the commit.
11. `git log -1 --format='%B'` and `git log -1 --format='%an <%ae> / %cn
    <%ce>'` — confirmed no AI attribution anywhere in the commit message
    and author/committer identity is the pre-existing local git config
    (`Yousef Wadi <ywadi85@gmail.com>`), untouched.

## Deviations from the brief

- `tools/fetch_slang_test.sh` needed `slangc -v 2>&1` instead of a plain
  stdout capture, because `slangc -v` writes to stderr. This is a fix
  within the spirit of the brief's own instruction to verify each step
  actually works rather than guessing — not a scope change.
- Root `CMakeLists.txt` diff is 4 lines (2 content + 2 blank separators for
  readability) rather than exactly 2; the two blank lines are cosmetic and
  don't change behavior. Content lines match the brief exactly:
  `include(tools/fetch_slang.cmake)` and `add_subdirectory(shaders)`.
- No other deviations. Archive layout matched the brief's `bin/slangc`
  guess after verification; slangc flags matched the brief's guess after
  verification; no flag adaptation was required.
- This report itself is filed at the worktree's copy of the path rather
  than the main-repo path named in the brief, because the sandbox blocked
  the direct write (see note at top of file).

## Self-review

- Re-read `tools/fetch_slang.cmake`, `shaders/CMakeLists.txt`,
  `shaders/tests/spirv_validity_test.cpp`, and the root `CMakeLists.txt`
  diff after writing them — logic matches what was actually verified above,
  no leftover debug code, no hardcoded absolute paths from this session's
  scratch probing.
- Confirmed `RX_TRIANGLE_VERT_SPV`/`RX_TRIANGLE_FRAG_SPV` are `CACHE
  INTERNAL` (not a plain `set`), per interface contract.
- Confirmed `RX_SLANGC` is a cache variable other CMake code
  (`shaders/CMakeLists.txt`) actually consumes, not just set and ignored.
- Confirmed the test macro-injection path
  (`target_compile_definitions(... RX_TRIANGLE_VERT_SPV="${...}")`) produces
  a working string literal — the test passed, so the quoting round-tripped
  correctly through Ninja's command escaping on both presets.
- Did not touch `src/**` or `third_party/CMakeLists.txt` — confirmed via
  `git show --stat HEAD` (only the 7 intended files changed).
- Symlinks `toolchain` and `.deps-cache` remain untracked in `git status`
  and are absent from the commit.

## Concerns / follow-ups for later tasks

- `shaders/shader_spirv_test.exe` under `windows-cross-zig` was built but
  not executed in this pass (no Wine invocation attempted here) — the ctest
  run confirmed above was only for `linux-native`. The toolchain file
  (`cmake/toolchains/windows-cross-zig.cmake`) does wire up
  `CMAKE_CROSSCOMPILING_EMULATOR` to `wine` when present, so `ctest --preset
  windows-cross-zig` should work if wine is installed on the box that later
  runs it — just flagging this wasn't separately exercised here since the
  brief's verification list only asked for build+configure on that preset.
- The Slang archive is fetched fresh into `third_party/slang-prebuilt/`
  (already git-ignored, confirmed pre-existing in `.gitignore` before this
  task) — nothing in this task added or needed to add a `.gitignore` entry.
- `tools/fetch_slang_test.sh`'s hardcoded `RX_SLANG_VERSION`/
  `RX_SLANG_PLATFORM` values must stay in sync with the same constants in
  `tools/fetch_slang.cmake` — there's no single source of truth shared
  between the CMake and bash layers (by design, matching the brief's scope
  of two separate scripts). Commented in both files pointing at each other.
- This report is filed in the worktree's `.superpowers/` copy, not the
  main-repo path the brief named — needs a manual copy/merge by whoever
  integrates this branch.
