# Task brief — OpenEXR (.exr) input support (issue #75, owner insertion between T10 and T11)

Requirements source: `gh issue view 75` (read it). This brief adds repo context the ticket cannot know.

## Scope
1. **Library-first decode.** tinyexr is the expected pick (single-file, zlib-licensed, the de-facto standard for lightweight EXR decode); do a brief written check against alternatives (OpenEXR proper = too heavy a dependency for one input path; miniexr is write-only) and record the call in your report. Vendor it the same way the repo's existing third-party single-header deps are vendored — investigate the current pattern (how stb/doctest/etc. arrive: `.deps-cache`/CMake fetch vs in-tree `third_party/`) and follow it exactly, pinned to a named release/commit.
2. **Integration point:** `rx_asset`'s texture decode path — the float-image (Radiance .hdr) dispatch added in P5 T6. Add an EXR sibling: detect by magic number (0x76 0x2F 0x31 0x01) with extension as a secondary hint, route to the EXR decoder, produce the same float RGBA payload the .hdr path produces, flowing into the same Environment-role → T9 bake chain untouched downstream.
3. **Scope bar (per ticket):** baseline scanline EXR, half and float, common compressions tinyexr fully supports. Partial/exotic variants (deep, tiled if unsupported, DWA/B44 if the vendored version lacks them) are REJECTED with actionable error messages naming the variant and the supported envelope — never silently-wrong pixels. Probe what the pinned tinyexr actually supports and set the rejection boundary honestly from evidence, not from its README.
4. **Sample 08:** `--env` accepts `.exr` (routing only — no new sample logic).

## Acceptance (all empirical, both drivers, driver-labeled)
- **Committed fixture:** generate a small procedural `.exr` fixture whose CONTENT is identical to the existing committed `.hdr` fixture (`samples/08_gltf_viewer/environments/gate_test_env.hdr`) — same generator, second container. Commit the generator script alongside. One-line provenance note per the test-assets policy (see `assets/test/ASSET-NOTES.md` conventions).
- **Container-equivalence proof:** decoded floats from the `.exr` fixture match the decoded `.hdr` within a tight epsilon (justify the epsilon: half-float quantization if the fixture is half). This is the float-fidelity assertion — it structurally rules out the LDR-collapse bug class the ticket cites.
- **Full-chain test:** decode → T9 bake → render through the same value-asserted path the .hdr fixture already exercises; the rendered result must match the .hdr-driven render within the same epsilon reasoning.
- **Rejection tests:** at least one unsupported-variant fixture (generated, e.g. deep or an unsupported compression) fails loudly with the actionable message; assert on the message content.
- **Revert-discrimination:** demonstrate at least one assertion fails against a deliberately broken decode (e.g. half-swap or channel-order sabotage), then restore green. Quantify.
- The byte-source grep CI check (`tools/` — find the script the CI "byte-source invariant" step runs) stays green; if the new decoder needs the sanctioned byte-source route, use it — do not exempt it.
- Full serial ctest on lavapipe AND real NVIDIA (RTX 2080 / 580.82.07 — label both), zero unfiltered validation errors. Wine-tier: run the CI-filtered subset if the touched code is in its scope.

## Process constraints (binding)
- Work ONLY in the worktree `/media/ywadi/second/renderer_x-worktrees/exr-support` (branch `task/exr-support`, base 9d65db3). `cd -P` into it — mandatory; a symlinked $PWD corrupts the dep-cache CMake key. Main checkout is read-only reference (SDD docs live there).
- NICE all builds/tests (`nice -n19`). NO forks/subagents. NO on-desktop windows — offscreen only.
- Commits on the branch only, explicit pathspecs, author = local git config, NO AI attribution of any kind, do NOT push.
- Report file: `/media/ywadi/second/renderer_x/.superpowers/sdd/2026-08-20-phase5-techniques/task-exr-report.md` — full detail there (library-choice rationale, pin, supported-envelope evidence, all proofs with numbers, per-driver test counts). Final message: status, commit hash(es), one-line test summary, concerns only.
