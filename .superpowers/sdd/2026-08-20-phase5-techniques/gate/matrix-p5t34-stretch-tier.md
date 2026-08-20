# Completeness matrix — P5 T34 (issue #70): Stretch tier — sheen (9), iridescence + dispersion (10), diffuse transmission (11)

**Plan task:** Task 34, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:897-919`).
**Charter binding:** priority order items (9)-(11) (`docs/superpowers/specs/
2026-08-09-toolchain-platform-rhi-design.md:452-458`), explicitly EXPLICITLY
stretch/registry-deferrable by the plan's own text. GI (priority 12) is
excluded from this task under any schedule outcome — not re-litigated here.

**Sources consulted (in-repo, 2026-08-20):**
- Repo-wide grep: zero hits for `sheen`/`Sheen`/`iridescence`/`Iridescence`/
  `dispersion`/`Dispersion`/`diffuseTransmission` under `shaders/` or
  `src/` — fully greenfield, as expected (this is downstream of Task 8's
  specialization-gated material slots and Task 11's conformance harness,
  neither landed at gate-research time).
- Task 11 (`plan:420-442`, Stage 1) is the harness this task's own
  acceptance criteria depend on ("gated via the Task 11 harness") — not
  yet built; this task cannot actually GATE anything until Task 11 exists,
  a sequencing fact worth stating plainly rather than assuming.

**Sources consulted (external, fetched 2026-08-20):**
- `KhronosGroup/glTF-Sample-Assets`, `main` branch, `Models/` directory
  listing (fetched 2026-08-20) — **all four conformance models the ticket
  names are CONFIRMED to exist verbatim**: `SheenChair`, `IridescenceLamp`,
  `DispersionTest`, `DiffuseTransmissionPlant`. (The plan's own citation
  here is accurate — unlike the Phase 4 DamagedHelmet/Sponza license
  corrections tools/fetch_assets.sh had to make, no correction is needed
  for these four model NAMES.)
- Per-model `LICENSE.md` fetched and read for each of the four:
  - `SheenChair`: model files CC0-1.0; metadata (README/metadata.json)
    CC-BY-4.0.
  - `IridescenceLamp`: CC-BY-4.0 (model + metadata).
  - `DispersionTest`: CC-BY-4.0 (model + metadata).
  - `DiffuseTransmissionPlant`: model files dual CC-BY-4.0 **and**
    CC0-1.0; metadata CC-BY-4.0.
  - None carry an NC (non-commercial) restriction or a EULA-style license
    like the Phase 4 Sponza/DamagedHelmet corrections had to flag — a
    clean set, no license correction needed in the fetch manifest this
    task will extend.
- `google/filament`, pinned tag `v1.75.0` — port-source verification per
  material, below (row-by-row).
- `KhronosGroup/glTF-Sample-Viewer` — named in the charter as the
  "material-vocabulary + reference-conformance source" for exactly this
  extension set (spec:380-383); not independently re-fetched this pass
  (Task 11's own gate matrix, a different ticket in this same primary-gate
  round, owns verifying the Sample Viewer's reference-render generation
  procedure in detail — flagged here only as a cross-reference, not
  re-verified to avoid duplicate work across the parallel gate agents).
- **Per-extension Filament presence, definitively resolved (`gh api
  search/code`, 2026-08-20, `repo:google/filament`):**
  - `KHR_materials_iridescence`: **zero hits, repo-wide.** Filament does
    NOT implement iridescence in any form (shader, material-import spec,
    or otherwise).
  - `KHR_materials_dispersion`: **real hits, confirmed implemented** —
    `shaders/src/surface_material_inputs.fs:80-81,188-189`
    (`#if defined(MATERIAL_HAS_DISPERSION) && (REFRACTION_TYPE ==
    REFRACTION_TYPE_SOLID)`, a real `material.dispersion` field wired into
    the solid/thick-volume refraction path), plus `libs/gltfio/materials/
    {volume,transmission,specular,sheen,base}.spec.in` (Filament's glTF
    material-import code-generation specs) and `README.md`/
    `RELEASE_NOTES.md` announcing the feature.
  - `KHR_materials_diffuse_transmission`: **zero hits, repo-wide.**
    Filament does NOT implement diffuse transmission.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Sheen/cloth (KHR_materials_sheen) | Filament cloth shading model — `shaders/src/surface_shading_model_cloth.fs` (confirmed present via this gate's own directory listing, `gh api repos/google/filament/contents/shaders/src`, fetched 2026-08-20; same file the charter names, "Filament cloth model + KHR_materials_sheen"). | Task-1-spec priority-slot decision (schedule-permitting, per plan's own stretch framing) | Confirmed the file exists at the pinned tag path (directory listing verified); full content not read this pass — **implementer must fetch and cite it directly at port time**, same discipline every other Stage-4 matrix applies to its own primary port source. | Port-parity vs. the named source + `SheenChair` conformance model gated via Task 11 (ticket's own text). |
| 2 | Iridescence (KHR_materials_iridescence, thin-film) | glTF Sample Viewer's own reference GLSL implementation — **the ONLY available precedent**: Filament confirmed to NOT implement this extension at all (Sources: zero hits repo-wide for `KHR_materials_iridescence`). | Task-1-spec priority-slot decision (schedule-permitting) | **Filament CONFIRMED ABSENT** (definitive, not a caveat — see Sources). The charter's blanket "Filament as canonical... clearcoat, anisotropy, sheen/cloth... IBL, refraction/absorption" framing (spec:371-376) does NOT extend to iridescence; the glTF Sample Viewer is the sole named port source for this specific feature. | Conformance: `IridescenceLamp`, gated via Task 11. Recommend the spec name the glTF Sample Viewer explicitly (not Filament) as this feature's port source, so nobody spends time searching Filament for it at implementation time. |
| 3 | Dispersion (KHR_materials_dispersion, rides the Task 23/24 transmission path) | Filament — **CONFIRMED implemented**, real production code (Sources: `surface_material_inputs.fs:80-81,188-189`, `MATERIAL_HAS_DISPERSION` gated on `REFRACTION_TYPE_SOLID`). | consume-now — primary port source resolved, this is NOT stretch-uncertain the way rows 2/4 are | Verified present and wired into exactly the pipeline slot the ticket's own text names ("rides the Task 23/24 transmission path" — Filament's own `REFRACTION_TYPE_SOLID` gating is the same thick-volume/Beer-Lambert path Task 24 ports). | Conformance: `DispersionTest`, gated via Task 11; port-parity vs. Filament's own dispersion formula (cited lines) in addition to conformance-model gating. Task 23/24's refraction machinery (Stage 3, landed before Stage 4 dispatches) is a stated prerequisite. |
| 4 | Diffuse transmission (KHR_materials_diffuse_transmission, leaves/wax) | glTF Sample Viewer's own reference GLSL — **the ONLY available precedent**: Filament confirmed to NOT implement this extension (Sources: zero hits repo-wide for `KHR_materials_diffuse_transmission`). | Task-1-spec priority-slot decision (schedule-permitting) | **Filament CONFIRMED ABSENT.** Same disposition as row 2 — Sample Viewer is the sole named source. | Conformance: `DiffuseTransmissionPlant`, gated via Task 11. |
| 5 | Variant discrimination (unused feature → zero cost) | Task 8's own already-specified specialization-bit/Slang-generics mechanism (`plan:342-370`, Stage 1, not yet landed). | consume-now (re-run of an EXISTING proof mechanism, not new design) | N/A — mechanism ownership belongs to Task 8; this ticket is a consumer/re-verifier. | Ticket's own text stands: "Task 8 proof re-run" per feature landed. |
| 6 | Checkpoint ruling for any deferred remainder | N/A — process discipline (plan's own "a recorded ruling, not a silent drop" text, plan:906). | consume-now | N/A. | Ticket's own text stands; recommend the ruling ALSO records, per feature, whether the Filament-vs-Sample-Viewer port-source question (rows 2-4) was actually resolved before deferral, so a future implementer picking this back up from the registry does not have to re-run this same verification. |

---

## Conflicts

**The charter's blanket "Filament as canonical... clearcoat, anisotropy,
sheen/cloth... IBL, refraction/absorption" framing (spec:371-376) does NOT
extend to iridescence or diffuse transmission** — both confirmed absent
from Filament by direct repo-wide code search (Sources). This echoes the
SAME class of finding this gate round already surfaced for T30 (Filament
has no froxel volumetric fog either) — the charter's Filament-covers-
everything framing needs a standing caveat, not a per-ticket rediscovery,
for every priority-9-through-11 stretch feature. Dispersion is the
exception: Filament DOES implement it, confirmed by real cited source.
Model NAMES/licenses (rows 1-4's conformance assets) are otherwise
accurate as written in the plan — no correction needed there.

## New gaps

None remaining — the "unverified Filament presence" gap this matrix
initially flagged for rows 2-4 has been resolved in-round (see Sources
and the per-extension search results): iridescence and diffuse
transmission are confirmed ABSENT from Filament; dispersion is confirmed
PRESENT.

## Open Questions (for the coordinator's binding ruling)

1. **Port source for iridescence and diffuse transmission (rows 2, 4):
   the glTF Sample Viewer, not Filament.** Definitively resolved this
   pass (not a caveat) — Filament ships neither extension (zero hits,
   repo-wide, for both `KHR_materials_iridescence` and
   `KHR_materials_diffuse_transmission`). **Recommendation: name the
   glTF Sample Viewer as the sole port source for these two** in the
   Task 1 spec, so the charter's general Filament framing does not send
   a future implementer looking for either feature in the wrong place.
   Dispersion (row 3) needs no such correction — Filament is a genuine,
   verified port source for it.
