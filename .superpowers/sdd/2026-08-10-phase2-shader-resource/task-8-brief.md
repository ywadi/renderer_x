### Task 8: CI + packaging for Phase 2 samples

**Files:** `.github/workflows/ci.yml` (extend), `samples/README.md` finalize.

Linux job: all new tests + all three sample headless gates under xvfb/lavapipe (same investigate-don't-skip rule as Phase 1). Windows job: everything builds; wine-run the non-GPU test set (rx_shader compiler tests run under wine ONLY if the Slang Windows DLLs load under wine — investigate; if they don't, exclude with an explicit workflow comment, never silently). Artifacts: all three samples per platform INCLUDING the Slang runtime libs + LICENSE for the hotreload sample (and any other sample that links rx_shader), laid out exactly as a user would unzip-and-run them [R:D2]. Budget check still passes with the grown codebase — if the warm build now exceeds 60s, report it (coordinator decides budget adjustment vs optimization; do not silently raise the number).
Push and `gh run watch` to green — a red run gets fixed in-task.

**Verify:** both jobs green on GitHub for real; artifact zips manually spot-checked (download one, run it on this machine). Commit clean.

---

