# SDD ledger — plan: docs/superpowers/plans/2026-08-09-toolchain-platform-rhi.md
Task 1: minor (deferred): tools/toolchain_check has no add_test() yet (expected — brief didn't call for one; first real ctest wiring lands with Task 4's rx_core_tests)
Task 1: minor (deferred): -Wno-nullability-completeness in zig wrapper scripts flagged by reviewer as maybe Linux-irrelevant — ruling: verified correct, zig's bundled libc++ headers trigger this warning regardless of target OS (confirmed during toolchain spike before writing the plan)
Task 1: complete (commits e6afc9e..416b919, review clean)
Task 2: minor (deferred): -Wno-nullability-completeness flag in windows wrapper scripts is a Clang/ObjC-nullability flag with no Windows/MinGW relevance — plan-mandated (brief specifies verbatim), harmless (unrecognized-for-target, silently inert), same root cause as Task 1's identical note
Task 2: complete (commits 416b919..11ad1b1, review clean)
Task 3: minor (deferred): cache key doesn't hash DEP_CMAKE_ARGS, so changing a cached dep's CMAKE_ARGS without bumping TAG would silently reuse the stale build (plan-mandated, inherited from brief's verbatim DepCache.cmake code)
Task 3: minor (deferred): rx_dep_cache_key's `zig version` execute_process has no RESULT_VARIABLE check (plan-mandated, inherited); low risk since Tasks 1-2 already validate the toolchain separately
Task 3: complete (commits 337b204..5f2b375, review clean)
Task 4: CRITICAL finding (Haiku implementer's commit contained forbidden Co-Authored-By/Claude-Session trailer, violating CLAUDE.md) — FIXED directly by controller via `git commit --amend` (message-only, tree/author unchanged, verified via git log + git diff --stat showing empty diff between old/new commit trees); not pushed yet at time of fix, so no public-history exposure. Commit hash changed 0eda1db -> b508684.
Task 4: minor (deferred): report's "Deviations from Brief: None" was inaccurate (doctest_main.cpp addition was an undisclosed-in-that-section but correct and necessary deviation) — no action needed, deviation itself was sound
Task 4: minor (deferred): Handle generation is uint32_t with no overflow guard (theoretical, not reachable in practice, not required by brief)
Task 4: complete (commits 5f2b375..b508684, 1 critical fixed directly by controller + verified, 2 minors deferred)
