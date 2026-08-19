# Helmet sampler-wrap fix brief (P0, supersedes the no-bug verdict)

Repo `/media/ywadi/second/renderer_x`, main checkout, base = current
HEAD (f67de3d; tree has only SDD workspace files uncommitted — not
yours). You are fixing the owner-reported DamagedHelmet rendering
defect, now root-caused by adversarial verification.

## Root cause (verified, reproduced)

- `shaders/material/material.slang:263-265` — `rx_sampleTexture()`
  samples EVERY material texture slot through one process-wide default
  sampler (`gSamplers[gMaterialGlobals.defaultSamplerIndex]`).
- `src/rx_material/material_system.cpp:1463-1471` — that sampler is
  hardcoded `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE` (a documented
  Task 4 seam-bleed choice; its own report predicted this gap:
  "per-material sampler/wrap selection is real, supportable future
  work... nothing tests it yet" — task-4-report.md:50-61).
- DamagedHelmet's `TEXCOORD_0` V lies entirely in [1.0005, 1.9987]
  (spec-legal; relies on glTF-default REPEAT; fastgltf defaults
  `Wrap::Repeat` for an empty sampler). Under CLAMP_TO_EDGE all V→1.0:
  the whole mesh samples each texture's bottom edge row (black dome,
  green jaw blob, emissive exactly zero).
- `TextureCache::getOrCreateSampler()` (rx_asset) ALREADY builds
  correct per-texture samplers honoring glTF wrap modes — currently
  orphaned; the shader never uses them.
- Proof chain (verifier): a build-tree `frac()` probe flipped the
  render to teal-visor/silver-panels matching an independent Blender
  no-IBL render (`helmet-reference-noibl.png` in this dir); a `v→3-v`
  flip test was byte-identical to the bug (clamp eats it); no flip
  transform exists anywhere in the pipeline; colorspace/decode layer
  verified clean; committed D17 reference encodes the bug; NO fixture
  in assets/test/ has UVs outside [0,1].

## Context reading

1. This brief. 2. `helmet-texture-fix-report.md` (the original
investigation you are correcting — its CPU-side audits were verified
CORRECT: decode fidelity, slot wiring, colorspace, slot-swap test; its
blindness: the UV gradient probe visualizes UV before the sampler, and
color-mean checks validate content-exists not position-correct).
3. Ledger entries in `progress.md` (search "helmet" and "Sampler").
4. The shader/material code paths cited above + `texture_cache.cpp`'s
sampler cache + `import_gltf.cpp:871-872` (sampler-state parsing) +
`draw_data.h`/param structs.

## The fix round — six items, all mandatory

1. **Wire per-slot samplers**: per-slot sampler bindless index through
   `StandardPbrParams`/`UnlitParams` (analogous to per-slot texture
   indices); `rx_sampleTexture` consumes it; the material-resolution
   path populates from TextureCache's already-built per-texture
   samplers; absent-sampler default = glTF-spec REPEAT (NOT clamp).
   Task 4's seam-bleed test that motivated CLAMP must now request its
   clamp sampler EXPLICITLY (update honestly; don't weaken asserts).
2. **The missing regression class**: committed fixture glTF with V in
   [1,2] + an asymmetric texture through the FULL import→render path
   asserting REPEAT semantics at specific pixels — scratch-worktree
   revert evidence mandatory (re-hardcoded clamp default must FAIL it).
3. **Shader-deploy DEPENDS** (mandatory — the hazard already
   contaminated the build dir in-session, causing a clean-HEAD
   sample_08 OutputNotConsumed failure): add DEPENDS on the `.slang`
   sources to BOTH deploy steps in `samples/08_gltf_viewer/CMakeLists.txt`
   (material_shaders ~:36-46 AND tonemap ~:58-63), and sweep every
   other sample's shader-deploy steps for the identical gap; fix all.
4. **Regen D17 references** (they encode the bug; sanctioned;
   provenance recorded) ONCE post-fix from a CLEAN rebuilt+redeployed
   build dir; refresh `helmet-after.png` at 512px — the owner reviews
   it personally.
5. **Report errata**: append a supersede section to
   `helmet-texture-fix-report.md` (original text stays; the addendum
   corrects: no-bug refuted; emissive-zero was the clamp, not camera
   framing; what the methodology could/couldn't see).
6. **Verify from a clean re-deployed build**: rx_asset + rx_material
   suites; serial linux ctest 22/22 (OutputNotConsumed contamination
   gone); windows-cross build; zero unfiltered validation errors incl.
   teardown-time (harness gate).

## Rules

Pathspec-scoped local commits (`git commit -- <paths>` — shared-tree
policy), NO AI attribution, author = local git config, no push, no
board/plan/spec/ledger edits, per-directory style, D5 one-liners on
any new public surface. An independent reviewer will empirically
revert-test your regression fixture — pre-empt with your own evidence.

## Report contract

Append your fix-round work to `helmet-texture-fix-report.md` (item 5's
addendum + evidence). FINAL MESSAGE: ONLY status, commit SHAs,
one-line test summary, concerns.
