### Task 11: glTF PBR conformance harness vs Khronos Sample Viewer

Stand up the conformance discipline the charter names: fetch Khronos
glTF-Sample-Assets conformance models (MetalRoughSpheres, EnvironmentTest,
EmissiveStrengthTest, TextureTransformTest, … — per-model licenses recorded
in fetch manifest), generate ground-truth reference renders from the Khronos
glTF Sample Viewer under matched camera/environment (generation procedure
scripted/documented + committed — the exact mechanism is a Task 1 gate
question), and gate our renderer against them with tolerance comparisons on
both drivers. Failures are findings to fix, never tolerance widenings.

**Files:** `tools/fetch_assets.sh` growth (sample-asset models + checksums),
`tools/` reference-generation procedure, `tests/` conformance gate suite,
committed references + provenance.
**Acceptance sketch:**
- ≥6 conformance models gated at Stage 1 close (core PBR + emissive +
  texture-transform set); the suite grows in Stages 2–4 as features land.
- MetalRoughSpheres within ruled tolerance on real driver; per-model
  discrimination proof (perturb roughness constant → gate fails).
- Ground-truth provenance (viewer version, camera, env, settings) committed
  next to every reference.
**Steps:** fetch + reference generation → gate harness → wire models →
discrimination proofs → both presets + real driver → commit.

