### Task 8: Sample 06_materials

**Files:**
- Create: `samples/06_materials/main.cpp`, `samples/06_materials/CMakeLists.txt`, `samples/06_materials/materials/checker.slang`, `samples/06_materials/materials/rim.slang`
- Modify: `samples/CMakeLists.txt`, `tools/package_samples.sh`, `.github/workflows/ci.yml`, `samples/README.md`, `MANUAL_VERIFICATION.md` (05+06 present-mode rows)

**Spec:** 4 objects (2 cubes, 2 spheres; reuse 03's procedural meshes) drawn through a single-pass rx_graph forward pass; materials `checker.slang` (UV checker × `tint`) and `rim.slang` (rim-light × `rimColor`), each instanced twice with DIFFERENT per-instance parameter values — all material creation and parameter setting go **exclusively through `rx_api.h` interfaces** (factory → `IRxMaterialSystem` → `IRxMaterial` → `IRxMaterialInstance`). Internal rx_material headers are confined to a clearly-marked bridge section: system creation (the desc bridge) and the draw-time `bindInstance`/`getPipeline` calls, which have no public equivalent in Phase 3 (the public surface covers the material model, not draw submission — D5/D11). Grep-able acceptance: every `#include <rx_material/...>` other than `rx_api.h` sits in the bridge section, and no `IRx*` object is ever cast to an internal type outside it. Headless: readback, assert the 4 known object-center pixels match 4 distinct expected colors (per-instance overrides visible). Present: orbit camera + `reloadChanged()` polling both material files each second, keep-last-good.

**Steps:**
- [ ] **1. Materials + sample; headless gate green** (`--validate`, zero validation errors).
- [ ] **2. Packaging + CI** (materials dir ships next to the binary + Slang runtime libs, as 02 does).
- [ ] **3. Both presets build; packaged-layout run verified.**
- [ ] **4. Commit** `feat: add sample 06_materials on the public material API`.

