### Task 9: Docs, deferred-minor fold-ins, roadmap

**Files:**
- Modify: `README.md` (Phase 3 → complete in Roadmap; project layout + samples list), `samples/README.md` (05/06 run instructions), `MANUAL_VERIFICATION.md` (if not finished in Task 8), `cmake/DepCache.cmake`, `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md` (tick delivered layers in the layer table)
- Create: `docs/abi.md` (the boundary rules of D5/R:M§1.3, for future contributors)

**Steps:**
- [ ] **1. DepCache fix** (carried deferred minor): include the dependency's `CMAKE_ARGS` in the cache-key hash (currently name|tag|triple|zig-version only) — changing CMAKE_ARGS must produce a new cache key; verify by reconfiguring with a changed arg and observing a rebuild, and document the key format in the file header comment.
- [ ] **2. Docs** listed above; keep hedged physical-hardware claims exactly as MANUAL_VERIFICATION.md does today.
- [ ] **3. Full local gate:** `ctest --preset linux-native --output-on-failure` all green; both presets build clean.
- [ ] **4. Commit** `docs: phase 3 documentation and dep-cache key hardening`.

---

## Execution notes (coordinator)

- Model assignment: Tasks 1-3, 5-7 Sonnet (multi-file/architecture); Task 4, 8 Sonnet (samples span shaders+cmake+CI); Task 9 Haiku (mechanical, fully specified). All reviews Sonnet.
- Parallelization: Task 4 (samples/05 + shaders/multipass) and Task 5 (src/rx_material + shaders/material) are file-disjoint after Task 3 lands → eligible for parallel worktree dispatch. Everything else is sequential.
- After Task 9: final whole-branch review (most capable model), at most one fix wave, then push, green CI, tag v0.3.0-phase3, attach CI packages, release notes with hedged hardware claims. Update this plan + ledger throughout.
