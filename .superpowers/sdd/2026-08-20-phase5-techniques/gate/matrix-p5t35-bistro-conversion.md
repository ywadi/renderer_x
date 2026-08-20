# Completeness matrix — P5 T35 (issue #71): Bistro conversion + hero-scene curation

**Plan task:** Task 35, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:921-945`).
**Charter binding:** *"Showcase/benchmark scene (committed 2026-08-19):
Amazon Lumberyard Bistro (NVIDIA ORCA distribution, CC-BY 4.0)... Requires a
one-time curated FBX/USD→glTF conversion (no official glTF exists;
conversion fidelity — alpha modes, normal-map orientation — is part of the
task)"* (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:464-474`).
Registry precedent: `progress.md:126` — "Bistro (ORCA, CC-BY 4.0, one-time
curated glTF conversion) recorded as techniques-phase showcase/benchmark
scene."

**Sources consulted (in-repo, 2026-08-20):**
- `tools/fetch_assets.sh` (full file read) — the EXISTING asset-fetch
  precedent this task extends: checksum-pinned per-file `sha256sum -c`
  verification, a versioned marker-file cache (self-invalidating on
  checksum-set bump), and a hard-won license-CORRECTION discipline (its
  own header comment documents that the ORIGINAL plan/matrix text called
  both DamagedHelmet and Sponza plain "CC BY 4.0", which was WRONG for
  both — DamagedHelmet actually carries a CC-BY-NC-4.0-tainted lineage,
  Sponza is under a Crytek EULA-style license, not Creative Commons at
  all). **This is the exact discipline T35 must apply to Bistro's own
  license claim** (verified below, row 1 — the charter's "CC-BY 4.0" claim
  for Bistro checks out independently, unlike the two Phase-4 precedents,
  but the VERIFICATION, not the assumption, is what this task inherits as
  its working method).
- `src/rx_scene/include/rx_scene/scene.h`/`draw_list.h` — the import
  target surface T35's conversion output must satisfy (glTF import
  already exists end-to-end since Phase 4; T35 is asset curation, not
  importer work, per the ticket's own "no importer rework" framing
  elsewhere in the plan for material-extension consumption, plan:459-463).

**Sources consulted (external, fetched 2026-08-20):**
- `developer.nvidia.com/orca/amazon-lumberyard-bistro` (WebFetch,
  2026-08-20) — **the ORCA page's own current asset listing, verified
  directly rather than assumed from the plan's own "FBX/USD" phrasing:**
  - **Available formats: FBX and "Falcor scene file format" ONLY. NO USD
    is offered.** The plan/ticket's "FBX/USD→glTF conversion" phrasing
    overstates the actual source options — there is no USD variant to
    choose between; the toolchain decision is FBX→glTF, full stop (see
    Conflicts).
  - License confirmed: Creative Commons CC-BY 4.0, matching the charter's
    claim exactly — **no correction needed here**, unlike the Phase 4
    Sponza/DamagedHelmet precedent.
  - Scene variants and triangle counts (useful for T36's benchmark
    sizing too): Interior 1,046,609 triangles; Interior-with-wine
    1,293,691 triangles; Exterior 2,832,120 triangles.
  - **No separate "night rig" ships in the source assets** — the gallery
    shows a nighttime render, but no distinct night-lighting scene
    variant or punctual-light placement data is part of the download.
    This directly confirms (not merely repeats) the ticket's own text
    ("punctual light placement for the night rig" is listed as part of
    the CURATION work) — the night rig must be AUTHORED by this task, not
    extracted from source data.
- `github.com/qian-o/GLTF-Assets/tree/main/Bistro` (WebFetch, 2026-08-20)
  — an UNOFFICIAL, already-published community FBX→glTF conversion of
  Bistro (CC-BY-4.0, derivative of the same ORCA source). Its own
  documented texture-channel convention is direct, LIVE EVIDENCE of one
  of the exact fidelity risks the charter names: **"Normal: DirectX normal
  map" is explicitly retained as-is** (i.e., NOT flipped to the OpenGL/
  glTF-standard tangent-space convention a naive/unaudited FBX→glTF pass
  would carry over unchanged) — this is not a hypothetical risk, it is
  observed in a real, already-shipped conversion of this exact source
  asset. Its "Specular: G=Roughness, B=Metalness" channel convention is
  worth noting for the material-disposition table: this ALREADY MATCHES
  glTF's own `metallicRoughnessTexture` channel convention (G=roughness,
  B=metallic), so that specific translation is a direct copy, not a
  remap — a small but concrete simplification for the disposition table.
  Two further unofficial conversions were found in a broader search
  (`vleue/bevy_bistro_playground`, `DGriffin91/bevy_bistro_scene`) —
  named here as additional cross-check fixtures, not independently
  fetched (diminishing marginal value past the first).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Source format reality check: FBX only, no USD | Charter/ticket text says "FBX/USD→glTF" (spec:470, ticket body). ORCA's own page (WebFetch, cited above) offers ONLY FBX (+ a Falcor-specific scene format, not a general interchange format). | consume-now (with a text correction) | Verified directly against the live ORCA page. | The toolchain decision is FBX→glTF; the acceptance criteria/spec text should drop the "/USD" branch entirely rather than leave a phantom alternative implying a choice exists. |
| 2 | Conversion toolchain: Blender FBX-import → Blender glTF-export | Plan's own text: "scripted/documented pipeline (Blender-based where scriptable)" (ticket body, plan:929). Blender ships BOTH a built-in FBX importer and the mature `io_scene_gltf2` glTF 2.0 exporter as first-party add-ons — no third-party tool needed for the core conversion. | consume-now — primary toolchain | N/A — this is a tool-selection row, not a code-port row; Blender's own bundled importer/exporter ARE the "ready-made" choice CLAUDE.md's don't-reinvent rule asks for, rather than hand-rolling an FBX parser or a glTF writer. | A scripted, documented, RE-RUNNABLE Blender pipeline (Python script driving `bpy.ops.import_scene.fbx`/`bpy.ops.export_scene.gltf`, matching the ticket's own "scripted/documented pipeline" text) — not a one-off manual GUI export nobody can reproduce or audit. |
| 3 | Normal-map orientation fidelity (DirectX vs. OpenGL/glTF tangent-space convention) | **LIVE, OBSERVED evidence, not a hypothetical**: the qian-o community conversion of this EXACT source explicitly documents retaining "DirectX normal map" convention unflipped (cited above) — direct proof that a naive conversion of Bistro's own FBX carries this risk forward. | consume-now (explicit, tested check) | N/A — this is a curation/QA row, not a library dependency. | A per-material (or per-texture) normal-map convention check as part of the disposition table (row 6): either the conversion pipeline flips the green channel during export/post-process, or the glTF material record correctly omits/sets whatever convention flag the renderer's normal-mapping code expects — verified by a VALUE probe (a known bump direction renders correctly lit), not by visual inspection alone, per the phase's own "reference-vs-ground-truth... never import-success-only" discipline. |
| 4 | Alpha-masked foliage | Charter/ticket text names this explicitly as a fidelity checkpoint. Bistro's exterior scene is well-known (from its wide use as a benchmark scene across the industry) to use alpha-tested/masked foliage cards — the qian-o conversion's own channel doc confirms "Alpha = Opacity" is carried in the BaseColor texture's alpha channel, consistent with a MASK-mode material once the glTF `alphaMode` is set correctly at conversion time. | consume-now | N/A — curation row. | Every foliage material's glTF `alphaMode` is explicitly set to `MASK` (never left `OPAQUE` or silently baked as `BLEND`) with a documented, tabled `alphaCutoff` per material — a discrimination test (foliage rendered with the wrong alpha mode produces a visibly/value-provably wrong silhouette) belongs in the material-disposition table's own verification, per the ticket's own text. |
| 5 | Glass storefronts mapped to REAL transmission extensions (never alpha blending) | Charter's own headline constraint, repeated at T35 specifically ("glass materials mapped to the real transmission extensions"). | consume-now | N/A — curation row; the CONSUMING extensions (`KHR_materials_transmission`/`volume`) are Task 23/24's job (Stage 3, landed before Stage 4 dispatches) — T35's job is correctly TAGGING which Bistro materials are glass so Task 23/24's machinery lights up on them, not re-implementing transmission itself. | Every glass material in the disposition table is explicitly tagged transmission/volume (never left as a plain alpha-blended `BLEND` material, which the charter explicitly forbids as the wrong representation for glass) — a value probe on at least one storefront pane (background visible through it at the correct IOR-predicted offset, per Task 23's own acceptance criteria) closes the loop end-to-end rather than stopping at "the material record LOOKS tagged correctly." |
| 6 | Emissive signage (strength units) | KHR_materials_emissive_strength (already consumed since Task 8, Stage 1, landed before Stage 4). | consume-now | N/A — curation row; consumption mechanism already exists by Stage 4. | Signage materials carry a documented, tabled emissive-strength VALUE (not left at the KHR default of 1.0 if the source content implies a brighter unit — cross-check against the source FBX's own emissive intensity data during conversion, not invented at random). |
| 7 | Punctual light placement for the night rig | Confirmed: **no such data exists in the source ORCA assets** (verified via the ORCA page fetch — no separate night-rig scene/light data ships). This is 100% authored curation work, not extraction. | consume-now (as new authored content, correctly already scoped as such by the ticket's own text) | N/A. | A documented, deliberate light-placement pass (count, type, intensity units per Task 13's physical-units model, Stage 2, landed before Stage 4) sufficient for T36's night-rig showcase; recorded as authored curation, not sourced-and-verified data, in the provenance notes. |
| 8 | Material-disposition table (every Bistro material → engine feature, tabled and committed) | Ticket's own explicit acceptance criterion. | consume-now | N/A — process deliverable. | Zero undispositioned materials; the table itself is the artifact rows 3-6 above feed into, committed alongside the conversion scripts. |
| 9 | Import-clean under Phase 4 gate rules (zero unknown-REQUIRED extensions) | Phase 4's own glTF import gate discipline (`gate/matrix-issue02-gltf-import.md` precedent, not re-read this pass — cross-referenced by name only). | consume-now | N/A — existing importer discipline, already delivered. | Ticket's own text stands. |
| 10 | Visual ground truth vs. ORCA reference renders (never import-success-only) | Phase 4's own "sponza-texture lesson" (named directly in the ticket body) — the exact precedent this criterion exists to avoid repeating. | consume-now | N/A — the ORCA page itself does not appear to publish a canonical reference-render set (WebFetch found gallery images, not a documented camera/settings-pinned reference procedure) — **flagged as a real gap**, see New gaps. | Matched-pose comparisons; the acceptance text should name WHAT the reference actually is (a self-generated reference from a documented, independent renderer/settings — e.g. the same glTF Sample Viewer or Blender's own Cycles/EEVEE render of the SOURCE FBX as an independent ground truth — see New gaps) since ORCA does not hand one over ready-made. |
| 11 | Distribution via `tools/fetch_assets.sh` (checksums; too large to commit) | The EXISTING pattern in the same file (rows for DamagedHelmet/Sponza/BoomBox), verified read in full. | consume-now | Verified: the exact "per-file sha256, versioned marker cache" pattern this task extends already exists and works; Bistro is large enough (multi-hundred-MB typical for this asset) to follow Sponza's own `--sponza`-gated, CI-never-downloads-large-assets precedent (`fetch_assets.sh:105-119` region) rather than DamagedHelmet/BoomBox's always-fetched-by-default treatment. | New `--bistro`-gated fetch function, same checksum/marker discipline, CI never downloads it by default (same reasoning as the existing Sponza gate: large, local/present-mode content). |

---

## Conflicts

**The plan/ticket's "FBX/USD→glTF conversion" phrasing (spec:470, ticket
body) is not accurate** — ORCA's Bistro distribution offers only FBX (plus
a Falcor-specific scene format, not a general interchange format); there
is no USD option to weigh against FBX. This is a small, non-blocking text
correction (row 1) — the actual toolchain decision this creates is
SIMPLER than the plan implies (one source format, not a choice between
two), not harder.

## New gaps

- **No documented, camera/settings-pinned reference-render procedure ships
  with the ORCA Bistro distribution** (row 10) — unlike, say, the Khronos
  glTF Sample Viewer's role for Task 11's conformance harness, there is no
  single authoritative "this is what correct looks like" image set for
  Bistro to diff against. The ticket's own "visual ground truth" criterion
  needs a NAMED independent reference generator before T35 can actually
  satisfy it, not just an instruction to "compare against ORCA reference
  renders" (plan:940) when ORCA does not, in fact, publish a pinned
  reference-render set on its asset page. Recommend the coordinator's
  ruling name a specific independent reference path — e.g., rendering the
  SOURCE FBX directly in Blender (Cycles, a physically-based path tracer,
  independent of this project's own renderer) under a documented
  camera/lighting setup, and diffing the CONVERTED glTF's render in
  RendererX against that Blender render — the same "independent
  ground truth, not self-referential" discipline Task 11's Khronos Sample
  Viewer comparison already uses for smaller conformance models.
- **Unofficial community glTF conversions of this exact asset already
  exist** (qian-o/GLTF-Assets, DGriffin91/bevy_bistro_scene,
  vleue/bevy_bistro_playground — all found via web search, 2026-08-20).
  None are recommended as the SHIPPED pipeline (see Open Questions) but
  qian-o's own documented texture-channel conventions are worth keeping
  as a cross-check fixture during T35's own conversion work — a second,
  independent data point on channel-packing/normal-convention questions,
  not a replacement for doing the curated conversion in-house.

## Open Questions (for the coordinator's binding ruling)

1. **Do the existing unofficial community glTF conversions of Bistro
   change T35's approach?** **Recommendation: no — perform the from-
   scratch curated Blender FBX→glTF conversion the plan/ticket already
   mandates, do NOT adopt any community conversion as the shipped asset.**
   Rationale: (a) the plan/charter text is explicit and deliberate that
   "conversion fidelity IS the task" — a third-party conversion of unknown
   provenance is exactly the kind of un-audited pipeline the Phase 4
   Sponza/sponza-texture lesson warns against; (b) at least one of the
   three found conversions (qian-o's) is ALREADY DOCUMENTED to carry
   forward the DirectX-normal-map fidelity risk the charter explicitly
   calls out as something to fix, not inherit; (c) none carries a
   documented, scripted, re-runnable pipeline this project could audit or
   maintain going forward (Sources: qian-o's own page does not name its
   tool/pipeline at all). Use qian-o's conversion only as a cross-check
   fixture per New gaps, above — never as the shipped pipeline or asset.
2. **Reference-render procedure for visual ground truth (row 10/New
   gaps).** Recommendation: an independent Blender Cycles render of the
   SOURCE FBX (documented camera/lighting/settings, committed alongside
   the conversion scripts) as the ground truth T35's own converted-glTF
   renders are diffed against — see New gaps for the full rationale. This
   keeps the "independent, not self-referential" discipline intact without
   requiring ORCA to publish something it does not.
