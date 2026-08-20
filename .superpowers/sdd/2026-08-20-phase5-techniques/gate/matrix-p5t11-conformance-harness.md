# Completeness matrix — ticket #47: [P5 T11] glTF PBR conformance harness vs Khronos Sample Viewer

**Plan task:** Task 11, "glTF PBR conformance harness vs Khronos Sample
Viewer" (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:420-442`),
Stage 1. **This ticket's own text explicitly names its hardest question as
unresolved: "the exact mechanism [for reference-render generation] is a
Task 1 gate question."** This matrix's headline finding directly answers
that question with evidence, not assumption.

**Binding sources:** techniques charter, glTF Sample Viewer as "the
material-vocabulary + reference-conformance source" (`toolchain-platform-
rhi-design.md:380-384`); Global Constraints reference-vs-ground-truth
discipline (plan:74-81, "gates that bake a bug certify the bug... every
reference regeneration carries provenance").

**Ticket body (`gh issue view 47`):** fetch Khronos glTF-Sample-Assets
conformance models (MetalRoughSpheres, EnvironmentTest,
EmissiveStrengthTest, TextureTransformTest, …); generate ground-truth
reference renders from the Khronos glTF Sample Viewer under matched
camera/environment (procedure scripted/documented+committed); gate our
renderer against them with tolerance comparisons on both drivers.
Acceptance sketch: ≥6 conformance models gated at Stage 1 close;
MetalRoughSpheres within ruled tolerance on real driver, per-model
discrimination proof; ground-truth provenance (viewer version, camera,
env, settings) committed next to every reference.

**Sources consulted (in-repo, read in full this session):** `tools/
fetch_assets.sh` (the established per-model license-verification
precedent — its own header comment documents a REAL, non-obvious license
correction it had to make for DamagedHelmet (CC-BY-4.0 **AND**
CC-BY-NC-4.0 combined, not plain CC-BY) and Sponza (CRYENGINE EULA, not
Creative Commons at all) — this ticket's "per-model licenses recorded in
fetch manifest" language is this project's own established discipline,
not new caution); repo-wide search for any existing conformance-test
harness (`src/rx_asset/tests/gltf_pipeline_test.cpp`,
`import_gltf_basisu_test.cpp`, `import_gltf_gpu_test.cpp` — these are
IMPORT-correctness tests, not render-conformance tests; no `tests/` tree
or CMake target for cross-renderer conformance exists anywhere in this
repository, confirmed by directory search).

**Sources consulted (external, fetched 2026-08-20):**
- `github.com/KhronosGroup/glTF-Sample-Viewer` (`gh api repos/...`):
  not archived, default branch `main`, license `Apache-2.0`.
- `glTF-Sample-Viewer/README.md` (fetched, full text read): confirms it
  is *"the official Khronos glTF 2.0 Sample Viewer using WebGL 2.0"* — a
  **browser application**, and its own supported-extension checklist
  independently confirms the "declared-but-gated" material set the T8
  matrix cites (clearcoat, sheen, anisotropy, specular, ior,
  transmission, volume, dispersion, iridescence, diffuse transmission,
  emissive strength — every one checked `[x]`).
- `glTF-Sample-Viewer/package.json` (fetched, full text read): `"test":
  "echo \"Error: no test specified\" && exit 1"` — literally no test
  infrastructure of any kind; `scripts.dev` runs `http-server` serving a
  Rollup-bundled Vue.js app; dependencies are `vue`, `bulma`,
  `@ntohq/buefy-next`, `gl-matrix`, `rxjs`, `gltf-validator` — a browser
  UI stack, zero CLI/Node-side rendering entry point, zero screenshot/
  Puppeteer/headless tooling anywhere in `dependencies`/
  `devDependencies`.
- `glTF-Sample-Viewer/src` directory listing (fetched): `logic`,
  `main.js`, `model_path_provider.js`, `ui` — confirms the repo's OWN
  code is UI-orchestration; the actual WebGL2 rendering logic lives in
  the separate `glTF-Sample-Renderer` submodule
  (`@khronosgroup/gltf-viewer": "./glTF-Sample-Renderer"` in
  `package.json`).
- `github.com/KhronosGroup/glTF-Sample-Renderer` (`gh api repos/...`):
  *"The render portion of Sample Viewer"*, license `Apache-2.0`, not
  archived — its `source/` tree (`GltfState`, `GltfView`, `Renderer`,
  `ResourceLoader`, `shaders`, `ibl_sampler.js`) is a real, separately-
  importable WebGL2 rendering LIBRARY, distinct from the Vue UI shell —
  see Open Questions for why this matters.
- `KhronosGroup/glTF-Sample-Assets` `Models/` directory (fetched):
  confirms `MetalRoughSpheres`, `MetalRoughSpheresNoTextures`,
  `EnvironmentTest`, `EmissiveStrengthTest`, `CompareEmissiveStrength`,
  `SpecGlossVsMetalRough`, `TextureTransformTest`,
  `TextureTransformMultiTest` all exist under `Models/` on `main` — every
  model the ticket names by name is real and present.
- Per-model `LICENSE.md` files (fetched, full text read, for
  `MetalRoughSpheres`, `EnvironmentTest`, `EmissiveStrengthTest`,
  `TextureTransformTest`): **`MetalRoughSpheres` = CC-BY-4.0.
  `EmissiveStrengthTest` = CC-BY-4.0. `TextureTransformTest` = CC0-1.0
  (public domain). `EnvironmentTest` = a proprietary "Adobe Stock
  License"** (its `LICENSE.md` points to `../../LICENSES/LicenseRef-
  Adobe-Stock.txt`, fetched — points to Adobe Stock's own "enhanced
  license terms" page, a commercial stock-content license, NOT an open/
  Creative-Commons license) **for the model content itself; only its
  `metadata.json` is separately CC-BY-4.0.**

---

## The matrix

| Requirement | Evidence | Disposition | Proposed acceptance criterion |
|---|---|---|---|
| Named models exist and are fetchable | All 4 named models (`MetalRoughSpheres`, `EnvironmentTest`, `EmissiveStrengthTest`, `TextureTransformTest`) confirmed present under `glTF-Sample-Assets/Models/` on `main`, each with a `glTF`/`glTF-Binary` variant. `EnvironmentTest` additionally ships a `glTF-IBL` variant subfolder — a real, pre-existing environment-map pairing directly useful for cross-testing against Task 10's IBL runtime path. | consume-now | Extend `tools/fetch_assets.sh` with the same versioned-cache/checksum pattern it already uses for DamagedHelmet/BoomBox/Sponza (script's own established idiom, not new tooling) — CI-fetched by default for the ≥6 models the Stage-1 acceptance bar names (the ticket names 4 explicitly; ≥6 means at least 2 more, unnamed — see Open Questions). |
| Per-model license verification | **Real, concrete finding, not a hypothetical:** `EnvironmentTest`'s model content is licensed under a proprietary Adobe Stock commercial license, not CC-BY-4.0 — directly analogous to (and independently confirming the SAME class of trap as) `fetch_assets.sh`'s own documented DamagedHelmet/Sponza license corrections. This repository is pushed to a PUBLIC remote (CLAUDE.md's own repository-policy header) — committing `EnvironmentTest`'s reference renders or re-derived assets under an ambiguous "Adobe Stock" license into public history carries real redistribution risk this project has already shown it takes seriously (the `fetch_assets.sh` header comment's own tone: "flagged prominently... WRONG for both, not just imprecise"). | consume-now, WITH A CAVEAT | `EnvironmentTest` is fetched on-demand for LOCAL/CI testing only (identical disposition to Sponza's own "OPTIONAL... CI NEVER downloads it... not committed to this repository" pattern, `fetch_assets.sh`'s own established precedent) — its glTF source AND any generated reference images are NOT committed to the repository; `MetalRoughSpheres`/`EmissiveStrengthTest` (CC-BY-4.0) and `TextureTransformTest` (CC0-1.0) are safe to commit references for (matching DamagedHelmet's own "small, fetched by default, gate-tested" disposition, modulo the CC-BY-NC caveat DamagedHelmet itself carries — attribution required, no additional NC-style restriction found on these three models' own LICENSE.md text this session). Every fetched model's license disposition is recorded in the fetch manifest per the ticket's own text — this is not new work, it is applying an already-proven pattern to 4 (soon ≥6) new models. |
| **Reference-render generation mechanism** | **Decisive finding: the Khronos glTF Sample Viewer has ZERO CLI/headless/scriptable rendering capability.** It is a browser-only WebGL2 Vue.js application (`package.json`'s own `"test": "echo... exit 1"`, `http-server`-served UI, no Puppeteer/Playwright/`headless-gl`/screenshot dependency anywhere in its manifest). There is no documented, supported way to invoke it non-interactively and capture a deterministic image. | **Genuinely unresolved by the port-source itself — this ticket must BUILD the generation mechanism, not merely invoke an existing one.** | See Open Questions below — this is the ticket's single largest undetermined-mechanism risk, and the ticket's own text already flags it as such. Not resolved here as a ruling (that is the coordinator's call), but narrowed to concrete, evaluated options with a recommendation. |
| Tolerance comparison methodology | Global Constraints' own "reference-vs-ground-truth discipline" (plan:74-81): *"every reference regeneration carries provenance... every new pixel gate ships with a discrimination proof."* This project's OWN established pixel-gate precedent (Phase 4's `damaged_helmet_test.cpp` and sample 08/09 gates — not re-read in full this session, cited by name from `fetch_assets.sh`'s own comment) is the direct in-repo template for "tolerance pixel gate" mechanics. | consume-now | Per-model tolerance is RULED (this ticket's own text: "MetalRoughSpheres within ruled tolerance on real driver" — a Task-1-adjudicated number, not invented per-model ad hoc), but the MECHANISM (perceptual diff metric vs. per-channel absolute tolerance vs. SSIM-style structural comparison) needs a single, documented, reused choice across all ≥6 models — recommend reusing whatever metric this project's existing sample-08/09 gates already use (consistency with established precedent, per the plan's own "no reinvent the wheel" standing rule) rather than introducing a new comparison library for conformance specifically, unless that existing metric proves too coarse for cross-RENDERER (not just cross-COMMIT) comparison — a real possibility worth a small pilot before committing to it for all 6 models. |
| Discrimination proof per model | Ticket's own text: "per-model discrimination proof (perturb roughness constant → gate fails)." | consume-now | Direct, mechanical: for at least `MetalRoughSpheres` (the metallic/roughness grid model — its own structure, an NxN sphere grid sweeping roughness/metallic, makes a "perturb roughness constant" test trivially well-defined, one of the reasons it is the charter's own headline conformance model), a deliberately-wrong roughness/metallic remap (e.g. skip the Disney/UE `roughness^2` alpha remap this project's OWN `standard_pbr.slang:197` already documents) must FAIL the gate — reusing exactly this codebase's own "revert-discrimination proof" standing convention (CLAUDE.md-inherited, cited throughout the SDD ledger). |

## Open Questions

- **Reference-render generation mechanism — RECOMMEND headless-browser
  automation (Playwright/Puppeteer driving a LOCALLY-BUILT `glTF-Sample-
  Viewer` `dist/` bundle, WebGL2 canvas screenshot capture) over the two
  weaker alternatives, with the generation script committed and
  version-pinned exactly like a code dependency.** Three real options,
  evaluated:
  1. **Headless-browser automation of the actual Sample Viewer app**
     (build the Vue app locally via its own documented `npm run build`,
     serve it, drive it with Playwright/Puppeteer setting camera/
     environment/model-selection state via its UI or its underlying
     `GltfView`/`GltfState` JS API directly — the `glTF-Sample-Renderer`
     submodule's `source/GltfView`/`GltfState` classes are a real,
     scriptable JS entry point BELOW the Vue UI layer, per the fetched
     directory listing — bypassing the UI entirely and driving the
     renderer library programmatically is very plausibly the cleanest
     path, though not independently confirmed this session by reading
     `GltfView`'s actual public API). RECOMMENDED: this is the ONLY
     option that generates references from the ACTUAL reference
     renderer's actual shader code (satisfying "ground-truth... never
     import/render success" in spirit — a genuinely independent
     implementation, not a re-derivation of what this project already
     believes is correct). Cost: adds a Node.js/Playwright toolchain
     dependency to this project's tooling (a real, non-trivial addition
     the "don't reinvent the wheel" rule actually argues FOR here, since
     Playwright/Puppeteer are exactly the "well-established library"
     CLAUDE.md's standing rule prefers over hand-rolling a WebGL
     screenshot harness) — scoped to the OFFLINE reference-generation
     step only (a one-time/occasional script, not a CI-per-commit
     dependency, since references are committed artifacts, regenerated
     only on a ruled discrepancy or a Sample-Viewer version bump).
  2. **Manually captured from the live deployed viewer**
     (`github.khronos.org/glTF-Sample-Viewer-Release/`) — REJECTED as
     the primary mechanism: not reproducible/scriptable, no committed
     procedure possible (violates the ticket's own "generation procedure
     scripted/documented + committed" requirement literally), and the
     deployed version drifts over time outside this project's control
     (no pinning). Could serve as a ONE-TIME manual cross-check of
     option 1's own output, never as the generation mechanism itself.
  3. **Reimplement/port the glTF-Sample-Renderer's shader math directly
     as a second internal reference path** (i.e. treat its WebGL2
     shaders as a second port source, like Filament, and evaluate them
     analytically rather than rendering through an actual browser) —
     REJECTED as the PRIMARY mechanism: this would make "ground truth"
     mean "our own reimplementation of Khronos's shaders," which
     collapses the independence this ticket exists to provide (a bug in
     BOTH this project's port AND its understanding of the reference
     shader would go undetected, whereas an ACTUAL rendered image from
     the actual reference app has no such shared-blind-spot risk). This
     option remains legitimate as a SUPPLEMENT (e.g., generating
     per-texel expected values for the DFG-LUT/white-furnace tests T7/T9
     already use, which are numeric, not image, ground truth, and
     inherently need a closed-form/independently-computed reference
     rather than a screenshot) — but not as this ticket's own
     image-conformance mechanism.
- **"≥6 conformance models" — only 4 are named by the ticket; RECOMMEND
  `MetalRoughSpheresNoTextures` and one of
  `SpecGlossVsMetalRough`/`CompareEmissiveStrength`/
  `TextureTransformMultiTest` as the additional 2.** `MetalRoughSpheres
  NoTextures` (confirmed present, same directory listing) is the
  lowest-risk fifth pick — same conformance value as `MetalRoughSpheres`
  itself (factor-only, no texture-sampling variables), useful as an
  ISOLATION case if the primary model's gate fails (distinguishes a
  texture-sampling bug from a core-BRDF bug). `TextureTransformMultiTest`
  is the natural sixth pick if `KHR_texture_transform` (already
  consumed per Phase-4's own gate ruling C4, cited in the Phase-4
  matrix-issue08) is in scope for this stage's conformance bar —
  recommend the coordinator confirm this rather than assuming it, since
  the ticket's own named 4 do not include it.
- **Camera/environment MATCHING procedure — not resolved this session,
  flagged as a real open mechanism gap regardless of which generation
  option (above) is chosen.** "Matched camera/environment" implies this
  project's OWN renderer and the Sample Viewer must agree on camera
  position/FOV/near-far AND (for `EnvironmentTest` specifically)
  environment orientation/intensity — neither app's camera CONVENTION
  (Y-up vs Z-up, FOV vertical vs horizontal, handedness) was cross-
  checked against the other this session. This is real, mechanical
  work the generation script (Open Question above) must get right
  once, then commit as reusable provenance (viewer version, camera
  params, env params — exactly what the ticket's own acceptance line
  already requires recording) — not a design decision so much as an
  implementation risk worth flagging so it is not discovered late.

## New gaps

- **No conformance-test CMake target/directory convention exists in
  this repository today** (`tests/` at the repo root does not exist as
  a distinct conformance suite location; only per-module `src/*/tests/`
  trees exist) — the ticket's own file list names a bare `tests/`
  directory (`plan:431-433`) which does not match this project's
  existing per-module test-tree convention anywhere else. Flagged for
  the coordinator: either this is a genuinely new top-level convention
  (a real architectural choice, since conformance tests span the
  material/asset/graph layers and don't belong to any one module), or
  the plan's own file-list wording is loose and this should land inside
  an existing module's `tests/` tree (`src/rx_material/tests` is the
  closest fit, given the ticket's material-focused acceptance criteria).
- **No Node.js/JavaScript toolchain dependency exists anywhere in this
  C++/CMake project today** — adopting Playwright/Puppeteer (Open
  Question's recommendation) would be the FIRST such dependency,
  meriting explicit "pin+license+windows-cross" recording per the
  plan's own Global Constraints (a real, first-instance decision, not a
  routine library add) — worth the coordinator's explicit sign-off given
  it is a new CLASS of toolchain dependency, not just a new library.

## Verification health

- `glTF-Sample-Viewer`/`glTF-Sample-Renderer` repo metadata, README, and
  `package.json` are FULL fetches (GitHub Contents API, base64-decoded),
  read in full this session — the "zero CLI/headless capability" finding
  is a direct reading of the actual dependency manifest and `test`
  script, not an inference from the README's marketing language alone.
- All 4 per-model `LICENSE.md` files were fetched and read in full this
  session (not search-digested) — the Adobe Stock License finding for
  `EnvironmentTest` is a first-hand read of that model's own
  `LICENSE.md` text, cross-referenced against the linked
  `LICENSES/LicenseRef-Adobe-Stock.txt` file (also fetched, though that
  fetch only returned the license's OWN pointer to Adobe's external
  terms page, not the full legal text of Adobe's enhanced license terms
  itself — the "commercial stock-content license, not open" framing is
  based on the license'S OWN NAME and referenced page, not a full read
  of Adobe's terms).
- `GltfView`/`GltfState`'s actual public API (the Open Question's
  "bypass the UI, drive the renderer library directly" recommendation)
  was NOT read in full this session — only the `source/` directory
  listing was fetched, confirming these files EXIST and are named
  plausibly, not confirming their actual scriptability. This is the
  single weakest citation in this matrix and the one most worth a
  follow-up fetch (`GltfView.js`'s own exported API) before the
  coordinator commits to the headless-automation recommendation as
  more than "most promising of three evaluated options."
- The existing Phase-4 pixel-gate tolerance-metric precedent (cited by
  name — `damaged_helmet_test.cpp`, sample 08/09 gates) was NOT
  independently re-read this session (out of this ticket's own file
  scope) — cited from `fetch_assets.sh`'s own comment and general
  session context, not freshly verified.
