### Task 12: Stage 1 exit — viewer upgrade + checkpoint numbers

`08_gltf_viewer` becomes the Stage 1 demonstrator using engine facilities
only: environment switching (`--env`), Task 4 exposure controls, HUD
environment/exposure readout; packaging/CI updated; stage checkpoint per the
Phase 4 pattern (suite green both presets + real driver, sample packaged +
standalone-verified, numbers in ledger: bake timings, frame times
helmet/sponza/workshop, driver-labeled).

**Files:** `samples/08_gltf_viewer`, `tools/package_samples.sh`, CI, README/
MANUAL_VERIFICATION rows, ledger.
**Acceptance sketch:**
- Viewer consumes only engine APIs (audit row per the Task 5 rule).
- Headless gates green incl. new env path; packaged zip standalone-verified.
- Stage numbers published (desktop, driver-labeled; Deck rows tracked).
**Steps:** implement → gates → package → numbers → commit.

---

## STAGE 2 — Lights + Shadows (charter priorities 3–4)

