# Task 16 brief — StandardPBR + Unlit + sample 08_gltf_viewer (card #8)

You are the implementer for Phase 4 Stage 1 Task 16 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
main checkout — base commit recorded at dispatch, tree clean except SDD
workspace files which are not yours). This task delivers the STAGE'S
USER-FACING SAMPLE — it ships in a downloadable prerelease immediately
after the stage closes, so polish and robustness of the sample itself
matter as much as the material library.

## Requirements — read IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 16:` INCLUDING "Added acceptance criteria" and
   "Gate hardening" blocks (BINDING; note the prose CORRECTION inside:
   alphaMode/doubleSided are pipeline fixed-function state per D28, NOT
   specialization constants).
2. Spec decisions D22, D28 (RC1 — lands HERE), D26.1, D17, D8, D10,
   D24, D25, D27:
   `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
3. Completeness matrix (acceptance criteria row by row — the biggest
   matrix of the stage, incl. the full glTF material model, every
   KHR_materials_* disposition, BRDF baseline, and the Conflicts
   section whose rulings are in the rulings file):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue08-standard-pbr.md`.
4. Coordinator rulings (+Errata; win on conflict):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — sections `RC1` and `**#8 StandardPBR (Task 16).**`.
5. Ticket body: `gh issue view 8` (amendment + GATE HARDENED blocks).

Order of authority: rulings (+errata) > spec > matrix > ticket.

## Landed context you build on (don't fight it)

- Task 13/14: `rx_asset` Registry + TextureCache are complete —
  materials arrive as parameter sets with resolved bindless texture
  handles (role-typed KTX2 path incl. KHR_texture_transform
  offset/scale carried per the C4 re-ruling — YOUR shaders apply it);
  D11 fallbacks; fixtures + fetch_assets.sh conventions.
- Task 15: async import is the default viewer path (rendered loading
  state); `cancel()`/teardown semantics are hardened — your sample's
  quit-during-load path MUST use them correctly, and the standing
  lesson applies: abandon/teardown paths get real-resource tests.
- Task 11: UploadTickets (poll on the frame loop). Task 10: memory
  accounting. Task 12: GeometryPool (element-unit suballocation).
- MaterialSystem (rx_material, Phase 3): `loadMaterial`/`getPipeline`/
  `bindInstance`, IMaterialShader Slang interface, specialization
  bits, VkPipelineCache. D28 is YOUR structural change to it.
- The closure sweep just landed (check `git log` at dispatch): GPU
  test binaries now FAIL on teardown-time validation errors — your
  tests inherit that gate.

## Scope summary (details in the matrix — this is a map)

- **D28 (RC1) — fixed-function pipeline-state axis on MaterialRecord**
  (blend enable + depth-write from alphaMode; cull mode from
  doubleSided; MASK cutoff carried as per-instance uniform with an
  always-present conditional discard), included in the pipeline cache
  key; `PipelineRequest` unchanged; two materials differing only in
  alphaMode/doubleSided yield distinct cached pipelines (cache-counter
  test). This is the reusable mechanism — document it as such.
- **`shaders/material/standard_pbr.slang` + `unlit.slang`** as ordinary
  public IMaterialShader modules (zero special treatment): full glTF
  metallic-roughness core per the matrix rows (baseColor/MR channel
  layout G=roughness B=metallic, normal map with BC5 Z-reconstruction
  + radicand clamp, occlusion `1.0 + strength*(occ-1.0)` closed-form,
  emissive, alphaMode x3, doubleSided), Lambertian diffuse (÷π
  normalization test) + GGX/Smith-correlated/Schlick specular, FG1
  interim flat ambient (uniform color × occlusion; closed-form
  non-black-metal probe), KHR_texture_transform offset/scale applied
  in-shader, MikkTSpace-consistent tangent basis (bitangent =
  cross(N,T.xyz)*T.w, sign test). Unlit = baseColorFactor ×
  baseColorTexture exactly, zero lighting dependence (light-flip
  zero-delta probe).
- **File-list growth (binding)**: `forward_entry.slang` + `material.slang`
  gain the tangent field + lighting surface (D8's vertex layout
  already carries tangents; the shader entry doesn't — prerequisite).
- **D26.1**: per-draw data via a bindless StructuredBuffer indexed by
  **`SV_VulkanInstanceID`** (NOT SV_InstanceID — Slang subtracts
  firstInstance; mandatory code comment + the two-draw
  firstInstance>0 test); legacy bindInstance/set-1 path stays for
  non-scene samples, explicitly scoped in comments.
- **`samples/08_gltf_viewer/`**: DamagedHelmet default (`--scene`
  override), ASYNC import by default with a rendered loading state,
  mouse-drag orbit (raw SDL via `Window::sdlWindow()` — sample-local,
  per the sequencing ruling; smooth orbit+zoom, sensible clamps),
  `--exposure` (pre-tonemap 2^exposure push constant; neutral-value
  regression guard proving shared tonemap shaders unchanged — you may
  NOT touch shaders/multipass/), `--vsync`/`--validate` flags per
  sample conventions, clean quit-during-load (cancel + teardown, with
  a real-resource test), headless gate per D17.
- **D17 infrastructure (NEW, yours)**: `tools/regen_references.sh`
  (documented, never auto-run) + committed 256x256 lavapipe reference
  PNGs; tolerance compare (±4/255/channel, <0.5% failing-pixel
  budget); TWO references: loading-state frame + loaded-scene frame;
  local-GPU divergence = info not failure. This mechanism is reused by
  sample 09 later — build it as a small reusable helper, not inline.
- **Packaging/CI**: sample 08 added to `tools/package_samples.sh`
  (binary + Slang runtime + LICENSE like 06/07 + PRE-STAGED
  DamagedHelmet with its CC-BY 4.0 AND CC-BY-NC 4.0 license texts and
  attribution — the fetch script already prints the corrected texts;
  the package must carry them as files next to the asset) + the
  stale header count fixed to include 08; CI headless gate wired like
  07's; MANUAL_VERIFICATION rows for the present-mode run.
- Discrimination standard: scratch-worktree revert evidence for the
  load-bearing tests most at risk (suggested: MASK-cutoff discard,
  BC5 Z-reconstruction, D28 distinct-pipeline cache counter,
  SV_VulkanInstanceID two-draw case, exposure-neutral guard).

## Global constraints (binding)

- **NO AI attribution of any kind** in commits; author stays local git
  config; conventional factual messages; commit locally; do NOT push;
  do NOT touch board/issues/plan/spec/ledger; only your own files.
- Production grade; TDD; suite green BOTH presets (serial linux ctest
  + windows-cross build + the Wine convention now incl. every binary
  CI runs); zero unfiltered validation errors INCLUDING teardown-time
  (new harness gate); per-directory style; D5 one-liners; GUID regen
  on ABI-visible material changes per the documented policy.

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-16-report.md`
(per-criterion proof vs the matrix, command output tails, revert
evidence, reference-PNG provenance, deviations, self-review). FINAL
MESSAGE: ONLY status, commit SHAs, one-line test summary, concerns.
