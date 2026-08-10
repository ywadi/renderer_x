# Task report: default sample validation off, ctest gates opt in with `--validate`

Status: **complete**. CI real green on GitHub. Commit `775769b`.

Final green run: https://github.com/ywadi/renderer_x/actions/runs/31369741216
(`linux-native` and `windows-cross-zig` jobs both succeeded).

## Bug

A user ran the released Windows sample bundle on a normal Windows machine with
no Vulkan SDK installed and got:

```
[error] [vulkan validation] windows_read_data_files_in_registry: Registry lookup
failed to get layer manifest files.
```

This is the Vulkan loader's own diagnostic for "no validation layers are
installed" — not a real validation error. It gets routed through this
project's debug messenger (`context.cpp`'s `debugCallback`, since it's a
`VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT`-or-above message that
doesn't match either of the two documented known-false-positive guards) and
counted by `hasValidationErrors()`, which the headless gate in every sample
treats as a failure.

Root cause: all four samples hardcoded `Context::create(extensions,
/*enableValidation=*/true)` at both their headless and `--present` call
sites. Validation is a developer/CI setting — it requires the Vulkan SDK (or
an equivalent `VK_LAYER_KHRONOS_validation` install) to do anything useful —
and has no business being force-enabled for an end user running a
redistributed release build.

## Fix

### 1. `Context::create` confirmed safe when validation is off

Read `src/rx_rhi_vk/src/context.cpp`'s `Context::create` first, per the
brief. Confirmed: `builder.request_validation_layers().set_debug_callback(...)`
is only called inside `if (enableValidation) { ... }` (lines 115-119). When
`enableValidation` is `false`, vk-bootstrap never registers a debug callback
and no `VkDebugUtilsMessengerEXT` is created at all — so a validation-off run
cannot see the loader's registry-lookup diagnostic (or any other validation
output) through this code path, regardless of what the loader itself logs to
its own channels. This is what makes "just default the flag to false" a
sufficient fix, with no engine-side change needed. Nothing in `context.cpp`
installs a messenger unconditionally.

### 2. All four samples: `enableValidation` defaults off, `--validate` turns it on

`samples/{01_triangle,02_hotreload,03_bindless_mesh,04_streaming}/main.cpp`,
identical shape in each file:

- `runHeadless()` / `runPresent()` signatures changed to
  `runHeadless(bool enableValidation)` / `runPresent(bool enableValidation)`.
- Both `Context::create(extensions, /*enableValidation=*/true)` call sites
  per file (headless + present) changed to
  `Context::create(extensions, enableValidation)`.
- Both `if (context->hasValidationErrors())` gate checks per file changed to
  `if (enableValidation && context->hasValidationErrors())` — the check is
  meaningless (and would silently always read "no errors reported" anyway,
  since no messenger exists) when validation was never turned on, so it's
  guarded rather than left to no-op accidentally.
- `main(argc, argv)`: added a `bool enableValidation = false;` alongside the
  existing `presentMode` flag, parsing a new `--validate` token the same way
  `--present` is parsed, then threading it into both `runPresent(...)` /
  `runHeadless(...)` calls. `--present` and `--validate` are independent and
  combine freely (e.g. `sample_01_triangle --present --validate`).

All existing behavior is byte-for-byte identical when `--validate` is passed
— the parameter just replaces what was previously a hardcoded literal.

### 3. ctest gates keep enforcing validation exactly as before

Each sample's `CMakeLists.txt` `add_test(NAME sample_XX_..._headless COMMAND
sample_XX_...)` changed to `... COMMAND sample_XX_... --validate)`, so
`ctest`/CI still runs every headless gate with validation layers active and
the zero-validation-error bar fully enforced — nothing weakened there. Only
the *default* behavior of the bare binary (what an end user gets) changed.

### 4. Docs

- `samples/README.md`: added a dedicated `--validate` bullet in the
  01_triangle section (developer/CI flag, requires the Vulkan SDK /
  `VK_LAYER_KHRONOS_validation`, combines with headless or `--present`);
  updated each sample's headless-mode description to state validation is off
  by default and that `ctest` passes `--validate` explicitly; fixed the
  01_triangle `--present` description ("survives resizes... with zero
  validation errors") which implied validation always runs; updated the
  `--present`-in-development. command comments under "Building and running"
  to clarify the bare command shown is validation-off and that `ctest` adds
  `--validate` on top; added a paragraph under "Running the automated test
  suite" spelling out that all four `ctest` cases pass `--validate` and why
  that's not the binaries' own default.
- `MANUAL_VERIFICATION.md`: the "What 'pass' means" bullet about
  `VK_LAYER_KHRONOS_validation` output now explicitly says to run with
  `--validate` (otherwise validation is silently off, which is correct for
  an end user but defeats the point of that checklist item). Did not alter
  the already-completed, dated Linux checklist entry (checked boxes + real
  hardware data from an earlier run, run before `--validate` existed) —
  rewriting a historical record's exact commands would misrepresent what
  actually ran; that entry predates this fix and is accurate for its own
  time. The still-open Windows/Steam Deck template sections were left as-is
  too (Windows doesn't carry the validation layer in this toolchain at all,
  so `--validate` there would be a no-op; Steam Deck's checklist doesn't
  itself check validation output).

## Verification

- `cmake --build --preset linux-native`: clean, 8/8 targets, no warnings from
  the changed files.
- `ctest --preset linux-native --output-on-failure`: **9/9 passed**, including
  all four `sample_*_headless` cases (`sample_01_triangle_headless`,
  `sample_02_hotreload_headless`, `sample_03_bindless_mesh_headless`,
  `sample_04_streaming_headless`), each now invoked with `--validate` per the
  CMakeLists change.
- Confirmed validation is genuinely still exercised by the gates: ran
  `sample_01_triangle --validate` directly and grepped its output — both
  documented known-false-positive warnings appear (`(known false positive:
  validation layer predates VK_KHR_portability_enumeration)` and `(known
  false positive: validation layer predates SPIR-V SourceLanguage=Slang)`),
  which only appear when the validation layers are genuinely loaded and
  running. This confirms the ctest gates are not accidentally validation-off.
- Ran all four sample binaries **without** `--validate` (the new default):
  `sample_01_triangle`, `sample_02_hotreload`, `sample_03_bindless_mesh`,
  `sample_04_streaming` — all four exited 0 with their own `"... PASSED"`
  gate line and **zero** `[vulkan validation]`-prefixed log lines of any
  kind (info, warning, or error) — confirming validation is genuinely off
  and no loader diagnostics get routed through the (nonexistent) messenger.
- `cmake --build --preset windows-cross-zig`: clean, 8/8 targets.
- Pushed to `main` (`775769b`); CI run
  https://github.com/ywadi/renderer_x/actions/runs/31369741216 finished
  **SUCCESS** on both the `linux-native` and `windows-cross-zig` jobs.
- Commit hygiene: `git log --format='%B' ecbe33d..775769b` contains no
  `Claude`/`Anthropic`/`Co-Authored-By`/`Generated` text; author is `Yousef
  Wadi <ywadi85@gmail.com>` (unchanged, no identity override).

## Concerns / follow-ups

- None blocking. One judgment call worth flagging: I extended the doc-update
  scope slightly beyond the literally-named `samples/README.md` to also
  touch `MANUAL_VERIFICATION.md`'s "what pass means" bullet, since that
  file's whole purpose is the pre-release `--present` validation check and
  it would otherwise have silently stopped exercising validation layers
  (default now off) with no note explaining why. I left its already-recorded
  historical Linux run entry untouched rather than rewriting a completed
  checklist's commands after the fact.
- Windows loader behavior (the actual triggering bug) could not be
  reproduced/re-verified on this Linux dev machine beyond the
  `cmake --build --preset windows-cross-zig` clean cross-compile — there is
  no real Windows hardware in this environment (consistent with
  `MANUAL_VERIFICATION.md`'s existing "not yet performed on real Windows
  hardware" note, unrelated to this task). The fix is verified by removing
  the *cause* (validation never turns on by default, so `Context::create`
  never installs a messenger and the loader's registry-lookup diagnostic has
  nothing to route through) rather than by reproducing the original failure.
