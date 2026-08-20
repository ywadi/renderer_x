# Texture-path round brief — runtime mips (Option A) + oversized-upload staging fix

Repo /media/ywadi/second/renderer_x, main at 9c76f01 (tree clean except
SDD workspace files, not yours). Owner-confirmed release-gate work: the
next release does not ship without both items. Build from the REAL path
only. STANDING RULE: real-NVIDIA runs (default ICD, driver-labeled)
alongside the lavapipe suite; lavapipe-only green is not verification.

## Item A — runtime mip-chain generation for the stb decode path (D10)

Prior analysis (sponza-visual-investigation.md §2.7 — read it): the
upload/registration path (`TextureCache::applyDecodeResult()`) is ALREADY
mip-chain-generic (KTX2 uses it), and the sampler cache already grants
full-chain trilinear for glTF MIPMAP min-filters. The missing piece is
generating levels inside `decodeStbForUpload()`.

Binding correctness requirements:
1. sRGB-role textures (baseColor, emissive): downsample in LINEAR space
   (decode sRGB -> average -> re-encode), never a naive byte average
   (visible per-level darkening). Slot roles are known at decode time —
   use them.
2. Normal maps: renormalize each averaged texel (flattening artifact
   otherwise).
3. Linear-data textures (metallicRoughness, occlusion): plain linear
   box average.
4. Library-first check: stb_image_resize2 is the obvious ready-made
   (already in the stb family; check third_party for it or vendor the
   single header, pinned) — prefer it over hand-rolled loops IF it can
   express the sRGB/linear and renormalize requirements; otherwise
   hand-roll the small kernels and say so.
5. Non-power-of-two dimensions handled per the standard floor-halving
   chain.
6. NEW correctness tests: known-pattern source -> assert expected
   filtered values at level 1+ for (a) sRGB path (prove linear-space:
   a naive-byte-average implementation must FAIL the expected values),
   (b) normal renormalization (prove unit length), (c) chain length.
   Revert-discrimination evidence for each.
7. D17 blast radius: every stb-consuming sample (06_materials,
   08_gltf_viewer, 09_scene grid) starts sampling real mip>0 content —
   regen references ONCE post-fix via tools/regen_references.sh
   (lavapipe, per convention), verify gates green, note provenance.

## Item B — oversized texture uploads (the 16MB staging cap)

Current defect: a texture whose mip-0 bytes exceed the staging capacity
(4096x4096 RGBA8 = 64MB > 16MB) is REJECTED and silently replaced by the
checkerboard placeholder (found on the Workshop asset, real NVIDIA).
Requirements:
1. Uploads larger than the staging window succeed via CHUNKED staging
   trips (multiple copy regions per texture through the existing ring —
   preferred; preserves bounded memory) or a documented dedicated
   transient staging allocation with D24 budget accounting. No unbounded
   growth of the ring itself.
2. Works composed with Item A (a 4k texture + its full mip chain in one
   logical upload).
3. Checkerboard fallback REMAINS for decode failures only — size is no
   longer a fallback reason.
4. Tests: a committed synthetic fixture exceeding the cap (e.g. a
   generated 4096x4096) through the full import->render path, asserting
   real content (not checkerboard) at specific pixels; revert-proof
   (re-cap the size, watch it fail). Real-world proof: the Workshop GLB
   (assets/fetched/Workshop/workshop_render_scene.glb, 31 materials, 4k
   textures) renders with real textures — capture one frame as evidence.
5. D25 upload-ticket semantics preserved (chunk trips must complete
   before registration; no host-wait-implies-device-visible assumptions).

## Verification bar

Full serial lavapipe ctest green; real-NVIDIA runs of sample 09 with
Sponza AND Workshop, sustained, zero unfiltered validation errors,
driver-labeled; windows-cross-zig build + Wine ctest; zero warnings;
revert-discrimination for every new load-bearing test; per-directory
style; D5 one-liners on new public surface.

## Rules

Pathspec-scoped local commits, NO AI attribution, author = local git
config, no push, no board/issue/plan/spec/ledger edits, FOREGROUND
commands with generous timeouts. An independent reviewer re-proves your
revert evidence and re-runs the real-driver checks.

## Report

Full report -> `.superpowers/sdd/2026-08-11-phase4-scene-assets/texture-path-round-report.md`
(per-item proof, before/after captures, mip-correctness test math,
command tails, deviations). FINAL MESSAGE: status, commit SHAs, one-line
test summary, concerns.
