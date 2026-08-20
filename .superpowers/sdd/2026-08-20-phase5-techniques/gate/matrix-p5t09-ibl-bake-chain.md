# Completeness matrix — ticket #45: [P5 T09] IBL bake chain — equirect→cubemap, SH irradiance, prefiltered specular, BRDF LUT

**Plan task:** Task 9, "Environment pipeline — equirect→cubemap, SH
irradiance, prefiltered specular, BRDF LUT (compute)"
(`docs/superpowers/plans/2026-08-20-phase5-techniques.md:371-395`), Stage 1.
**This is the critical dependency-check ticket the dispatch brief named
explicitly** (item c: "the compute IBL bake chain's Vulkan requirements
vs Stage 0 T2's planned capability"). The finding below is the headline
result of this matrix: **T9's requirements exceed what Task 2's plan text
and the render graph's CURRENT resource model promise, by a wide margin,
in a way that is not a minor gap but a load-bearing architecture
question.**

**Binding sources:** techniques charter environment/indirect paragraph
(`toolchain-platform-rhi-design.md:422-427` — "at least as important as
the BRDF... HDR environment → SH (or irradiance-cubemap) diffuse +
prefiltered specular cubemap with roughness-selected mips + BRDF-
integration LUT"); Task 2 (`plan:173-205`, full text read); Task 6
(`plan:286-309`, full text read — the Stage-0 cubemap/array KTX2 LOADING
task, a distinct capability from this ticket's WRITE/bake need, see
below); Global Constraints (compute passes through the graph's
compute-class machinery, no hand-rolled dispatch, plan:109-111).

**Ticket body (`gh issue view 45`):** equirect→cubemap conversion;
diffuse irradiance (SH9 or irradiance cubemap, spec ruling; Filament
`cmgen` the port source either way); prefiltered specular cubemap with
GGX importance-sampled roughness-per-mip; DFG BRDF-integration LUT; runs
at load time on GPU through render-graph compute passes, results cached
per environment; offline baking deferred to Phase 7 (registry pointer).

**Sources consulted (in-repo, read in full this session):**
`src/rx_graph/include/rx_graph/resources.h` (`AttachmentDesc`,
`BufferDesc`, `ResourceAccess`, `PhysicalResource` — full struct
definitions), `src/rx_graph/include/rx_graph/pass.h` (every `Pass::add*`
declaration — `addColorOutput`, `setDepthStencilOutput`,
`addTextureInput`, `addStorageBufferOutput`/`Input` — grepped exhaustively,
**no** `addStorageImageOutput`/`addStorageImageInput` exists),
`src/rx_graph/transient_pool.cpp` (`TransientPool::acquireImage()`,
:15-45 — `Texture2D::create(..., /*requestedMipLevels=*/1)` hardcoded),
`src/rx_graph/executor.cpp` (barrier construction at :534-536, :705-707,
:807-809, :831-833 — every subresource range uses `baseMipLevel=0`/
`levelCount=VK_REMAINING_MIP_LEVELS`/`baseArrayLayer=0`, i.e. whole-
resource barriers only, no per-mip/per-layer subresource addressing
anywhere in the executor), `src/rx_rhi_vk/include/rx_rhi_vk/texture.h`
(`Texture2D` class — `mipLevels()`/`createForPresuppliedMips()` exist;
grepped for `arrayLayers`/`cube`/`Cube`/`VK_IMAGE_VIEW_TYPE` — zero hits,
no array-layer or cube-map concept anywhere in this class), `src/
rx_material/material_system.cpp` (:1930-1936, the attachment-free-
signature rejection Task 2's own plan text names).

**Sources consulted (external, fetched 2026-08-20):**
- `libs/ibl/src/CubemapIBL.cpp` (`google/filament`, fetched via `gh api`,
  full relevant sections read): `CubemapIBL::roughnessFilter()` (GGX
  importance-sampled specular prefiltering — the `linearRoughness==0`
  special case is a literal `cm.sampleAt(N)` passthrough, i.e. mip-0 IS
  exactly the source, not an approximation — direct citation for the
  ticket's own "mip-0 ≈ source" acceptance line), `CubemapIBL::
  diffuseIrradiance()` (:554), `CubemapIBL::DFG()` (:1008, `multiscatter`/
  `cloth` bool toggles — see the T7 matrix for the energy-compensation
  cross-reference).
- `libs/ibl/src/CubemapSH.cpp` (fetched, function signatures read):
  `CubemapSH::computeShBasis()`, `CubemapSH::windowSH()` (cites Peter-
  Pike Sloan, "Deringing Spherical Harmonics" — SH ringing mitigation,
  available precedent, not necessarily Stage-1-required),
  `CubemapSH::renderSH()`, `CubemapSH::preprocessSHForShader()` (cites
  Sloan, "Stupid Spherical Harmonics (SH)").
- `tools/cmgen/README.md` (fetched, full text read): confirms `cmgen`
  consumes equirect/cross-cubemap HDR input (PNG/Radiance `.hdr`/PSD/EXR)
  and PRODUCES **KTX1** output (quoted: *"KTX files are always KTX1
  files, not KTX2... encoded with 3-channel RGB_10_11_11_REV data"**) —
  this project's own committed convention is KTX2-first (D10,
  `phase4-scene-assets-design.md`) — `cmgen` is a PORT-SOURCE-FOR-
  ALGORITHMS reference (CPU offline tool), never a runtime dependency or
  file-format precedent; this ticket ports its MATH into GPU compute
  kernels, not its I/O format.
- Vulkan spec (`docs.vulkan.org`, WebFetch attempts) + WebSearch digest:
  no explicit VUID prohibits a `VK_IMAGE_VIEW_TYPE_CUBE`/`_CUBE_ARRAY`
  image view from carrying `VK_IMAGE_USAGE_STORAGE_BIT` (confirmed
  requirements: `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` at image creation,
  `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` on the chosen format, identity
  component swizzle for storage descriptors) — see Open Questions below
  for why this project should still prefer 2D-ARRAY views for compute
  WRITES regardless of what is spec-legal.

---

## The matrix

| Bake stage | Filament port source (pinned — see T7 matrix for resolved commit) | Vulkan/render-graph capability actually needed | T2's promised capability (plan:173-205, quoted/paraphrased) | Gap? |
|---|---|---|---|---|
| Equirect → cubemap conversion | `CubemapUtils.cpp` (equirect sampling into cubemap texel directions — not fetched in full this session, file listed and its role confirmed via `Cubemap.cpp`/`CubemapUtils.cpp` presence in `libs/ibl/src`). | A compute pass that READS a 2D equirect texture (bindless, sampled — T2 promises "bindless set 0 accessible from compute", satisfied) and WRITES 6 cube FACES of a destination image (array-layer-indexed storage writes, one dispatch region per face or one dispatch with a face axis in the invocation ID). | "a storage-buffer AND a storage-image write" (singular, unqualified — the acceptance sketch's own GPU test writes to what reads as ONE 2D storage image, not an array). | **YES — array-layer-indexed storage image writes are not named anywhere in T2's acceptance sketch or file list.** |
| Diffuse irradiance (SH9 or irradiance cubemap — spec ruling) | `CubemapIBL::diffuseIrradiance()` (direct convolution) OR `CubemapSH::computeShBasis()`+`windowSH()`+`preprocessSHForShader()` (SH9 path) — **the ticket's own text defers SH-vs-cubemap to Task 1's spec ruling**, not re-litigated here. | **SH9 path:** a REDUCTION — thousands of texel samples across a source cubemap accumulated into 9 small `float3` coefficients. Standard GPU implementations use either (a) atomic float accumulation into a small storage buffer (`atomicAdd` on a float-typed buffer element — core GLSL/HLSL atomics are INTEGER-only; float atomics need `VK_EXT_shader_atomic_float`, an OPTIONAL device extension, or a manual compare-exchange/fixed-point-bitcast workaround), or (b) a multi-pass parallel reduction (dispatch N, write partial sums to a buffer; dispatch N/64, reduce again; ...) — architecturally a CHAIN of compute passes each reading the previous pass's storage-buffer output. **Irradiance-cubemap path:** avoids the reduction entirely (each destination texel independently convolves the source — embarrassingly parallel, no atomics/reduction needed) but still needs the array-layer-indexed cube-write capability from the row above. | Storage-buffer write/read chaining is explicitly promised ("a downstream pass reads both" — buffer AND image). Atomics/multi-pass reduction chaining is **not mentioned at all**; float-atomic device-extension support is not named anywhere in Task 2's or the toolchain doc's device-feature enablement lists (grepped — no hit). | **YES for the SH9 path specifically (atomics/reduction-chain capability + a possible optional-extension enablement decision); NO additional gap for the irradiance-cubemap path beyond the equirect→cubemap row's own array-write gap** — this is itself a real, material input to the SH-vs-cubemap spec ruling: the irradiance-cubemap path is STRICTLY CHEAPER on Task-2-dependency grounds, independent of its own well-known runtime/memory tradeoffs (SH9 is 9 floats vs. a full filtered cubemap). |
| Prefiltered specular cubemap (GGX importance-sampled, roughness-per-mip) | `CubemapIBL::roughnessFilter()` — one call PER MIP LEVEL, each with a different `linearRoughness` (the "roughness-per-mip" the ticket names), reading the FULL source mip chain (`levels` parameter — a `Slice<const Cubemap>`, i.e. this convolution itself needs MULTIPLE source mips as read input, not just mip-0) and writing ONE destination mip. | A compute pass PER MIP that (a) reads a specific mip range/level of the SOURCE cubemap (a sampled/bindless read scoped to a subresource — needs a view of a specific mip, not the whole chain) and (b) writes to a specific mip LEVEL of the DESTINATION cubemap (storage write scoped to `baseMipLevel=N, levelCount=1` — a "view a single mip of a multi-mip image as its own storage target" capability) across all 6 faces (array-layer writes again). | Nothing beyond the single-mip, single-layer "a storage-image write" already cited above. `TransientPool::acquireImage()` hardcodes `requestedMipLevels=1` — a transient/pooled render-graph image CANNOT have more than one mip today at all, let alone be written to at a specific non-zero mip via a distinct view. | **YES, the largest gap in this matrix.** This is not merely "T2 doesn't mention it" — it is "the render graph's resource model structurally cannot represent a multi-mip image as a pass resource today" (see the in-repo citations above: `AttachmentDesc` has no mip-count field, `TransientPool` hardcodes 1, `Texture2D` has no array/cube concept). This SAME capability (per-mip storage-image write scoped to a subresource) is independently needed by Task 22 (Stage 3, "Scene-color HDR mip chain") and Task 31 (Stage 4, "Bloom") — **this is not a T9-specific ask, it is a render-graph-wide missing primitive that three separate Phase-5 tasks independently need**, making it a strong candidate for landing ONCE, generically, rather than three times, ad hoc. |
| BRDF-integration (DFG) LUT | `CubemapIBL::DFG(js, dst, multiscatter, cloth)` (:1008) — a flat 2D `Image`, NOT a cubemap, dimensions `(NoV, sqrt(linearRoughness))`. | A compute pass writing a single 2D storage image, single mip, single layer — no array/cube/multi-mip capability needed at all. | Exactly matches "a storage-image write" as literally described. | **NO gap** — this is the one bake stage that fits T2's promised capability as-written, unmodified. |

## Open Questions

- **The render-graph mip/array/cube resource-model gap — RECOMMEND
  extending `AttachmentDesc`/`TransientPool`/`Pass`'s storage-resource
  API generically in Task 2 itself (widening Task 2's own file list),
  rather than treating it as T9-local scope creep.** Evidence for
  "generic, not T9-local": Task 22 (HDR mip chain) and Task 31 (bloom)
  both need multi-mip transient images independently of IBL; a
  cubemap/array need also surfaces at Task 6 (Stage 0, KTX2 cubemap/
  array LOADING — read-only, sampled-usage, but the underlying
  `Texture2D`/RHI-level array-layer gap is the SAME missing primitive,
  just on the read side) and Task 18 (spot shadow atlas / point shadow
  cubemap, Stage 2). Landing a real `arrayLayers`/`mipLevels`-aware
  storage-image resource description ONCE (in Task 2, since it already
  owns "compute pipeline capability... lifting the attachment-free-
  signature rejection") avoids at least four later tasks each solving a
  narrower slice of the same problem, which is exactly the "retrofit-
  expensive" pattern CLAUDE.md's fast-path-as-default rule (and this
  plan's own D26.4/BDA-enablement precedent from Phase 4 — enable the
  capability once, opportunistically, ahead of the first real consumer)
  argues against. This is a genuine scope/sequencing decision for the
  coordinator (does Task 2's file list widen, or does T9 carry its own
  narrower version and Tasks 22/31/6/18 each extend it further later) —
  recommending the former on retrofit-cost grounds, not asserting it as
  already-ruled.
- **Cube-typed storage image views vs 2D-ARRAY views for compute
  WRITES — RECOMMEND 2D-ARRAY views for every compute WRITE target in
  this bake chain, reserving CUBE-typed views for later SAMPLING
  (fragment-shader IBL consumption, Task 10) only.** No spec text
  surfaced this session flatly prohibits a Cube/Cube-Array storage image
  view (see Sources — the requirements found were affirmative:
  cube-compatible creation flag, storage format-feature bit, identity
  swizzle), so this is NOT a hard spec blocker either way. The
  recommendation rests on PRACTICE, not a spec prohibition: every
  Filament/industry cubemap-compute-generation reference this session's
  research touched (`CubemapIBL.cpp`'s own CPU-side `Cubemap` abstraction
  treats faces as independently addressable 2D surfaces, not a single
  `imageCube` blob) and the general real-time-rendering convention is to
  address cubemap faces as `image2DArray` layers (`gl_GlobalInvocationID.z`
  selects the face) for WRITES, and only construct a genuine
  `VK_IMAGE_VIEW_TYPE_CUBE` view of the SAME underlying `VkImage` for
  later hardware-cube-sampling (trilinear-across-face-seam filtering,
  which only cube-typed SAMPLED views get) — this is a two-views-one-
  image pattern (one 2D_ARRAY view for compute write, one CUBE view for
  fragment-shader read), not a single view serving both purposes. This
  needs verification-in-task (a throwaway compile+dispatch probe,
  matching this project's own established "verified directly against
  the project's shipped Slang/Vulkan build before relying on it"
  discipline — forward_entry.slang's own header comment is the
  precedent for this kind of pre-flight check) before the coordinator
  treats it as settled; recorded as a recommendation with a clear
  verification step, not a claim.
- **SH9 float-atomics — RECOMMEND the irradiance-cubemap path over SH9
  for Stage-1 SPECIFICALLY on this dependency-cost basis, without
  overriding the ticket's own "spec ruling" framing for the
  quality/runtime-cost tradeoff itself.** This matrix's own finding (SH9
  needs either an optional device extension or a multi-pass reduction
  chain; irradiance-cubemap needs neither, beyond the array-write gap
  every path already needs) is a NEW, concrete input to Task 1's spec
  ruling that the ticket text's "SH9 or irradiance cubemap" framing does
  not currently weigh — recommending the coordinator fold this into
  whichever ruling lands, not asserting the ruling itself (there are
  real, legitimate reasons to prefer SH9 regardless — 9 floats vs. a
  full filtered cubemap is a meaningful runtime-memory/bandwidth win on
  the Steam Deck floor CLAUDE.md's performance-first posture cares
  about — so this is ONE factor, not a slam-dunk).
- **Does Task 2 need to land BEFORE this dependency-check even makes
  sense, i.e. is the gap analysis premature?** No — Task 2's own plan
  text is read as WRITTEN (its acceptance sketch, not speculation about
  what its implementer might additionally build), and the finding is
  that the WRITTEN acceptance bar undershoots T9's real need. If Task 2
  lands with a broader resource model than its own plan text currently
  promises (the coordinator widening its scope per the recommendation
  above), this gap closes automatically — the matrix's job is to make
  the undershoot visible NOW, before Task 2 dispatches with a narrower
  acceptance bar that then blocks T9 on arrival.

## New gaps

- **`rx_rhi_vk::Texture2D` has no array-layer or cube-map concept at
  all** (grep-verified this session — no `arrayLayers` constructor
  parameter, no `VK_IMAGE_VIEW_TYPE_CUBE`/`_2D_ARRAY` anywhere in
  `texture.h`). This is a LOWER-LAYER gap than the render-graph one
  above — even outside the graph's transient-pool system, a hand-rolled
  compute pass could not create a cube/array `Texture2D` today at all.
  Task 6 (Stage 0, cubemap/array KTX2 LOADING) is the most likely place
  this lands for the READ/sampled-usage case (loading a pre-baked file);
  this ticket (T9) needs the WRITE/storage-usage case additionally,
  which Task 6's own plan text (loading via "the existing chunked-
  staging path") does not cover either — chunked staging is an UPLOAD
  path for texture DATA already resident on the host, not a mechanism
  for allocating an empty GPU-writable render target. Proposed fit:
  whichever task extends `Texture2D` for array/cube support (Task 6 is
  the natural owner since it is Stage 0 and sequenced first) should
  extend it far enough to cover BOTH the sampled-read case (Task 6's own
  need) and the storage-write case (this ticket's need) in one pass,
  rather than Task 6 building a narrower version T9 then has to widen.
- **No existing precedent anywhere in this codebase for a multi-pass
  compute REDUCTION (dispatch → intermediate buffer → dispatch again)**
  — relevant only if the SH9 path is chosen (Open Question above); not
  a gap if the irradiance-cubemap path is chosen instead.

## Verification health

- Every render-graph/RHI capability claim (`AttachmentDesc` fields,
  `TransientPool::acquireImage()`'s hardcoded mip count, the executor's
  whole-resource-only barrier ranges, `Pass`'s missing storage-image
  API, `Texture2D`'s missing array/cube support) is a DIRECT grep/read
  of the current working tree this session, not inferred from the plan
  or spec documents — the plan/spec never claim these capabilities
  exist, so this is new, independently-verified evidence, not a
  contradiction of anything already written.
- Filament `CubemapIBL.cpp`/`CubemapSH.cpp` citations are full-file
  fetches (GitHub Contents API, base64-decoded), function-signature and
  key-logic level reads, not exhaustive line-by-line ports — sufficient
  to confirm WHICH functions are the port source and their high-level
  data-flow shape (per-mip calls, reduction vs. direct-convolution
  shape), not sufficient to claim every internal numerical detail was
  verified (that is the porting task's own job, not this gate's).
  Pinned to the same Filament commit as the T7 matrix.
- `cmgen`'s KTX1-output finding is a direct quote from its own README,
  fetched this session — the inference that this project's runtime
  compute port needn't inherit KTX1 (since it never touches `cmgen`'s
  file I/O, only its math) is this matrix's own reasoning, not an
  external citation.
- The Vulkan storage-image-cube-view spec research was INCONCLUSIVE by
  direct fetch (WebFetch against `docs.vulkan.org` returned truncated/
  unhelpful excerpts on three attempts, noted honestly rather than
  papered over) and rests on a WebSearch digest for the specific VUID
  text quoted (`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`, `VK_FORMAT_
  FEATURE_STORAGE_IMAGE_BIT`, identity-swizzle requirements) — treat the
  "no explicit prohibition found" claim as WEAKER evidence than the
  in-repo grep findings above; the Open Question's recommendation
  (2D-ARRAY for writes) is deliberately hedged with an in-task
  verification step for exactly this reason, not asserted as settled
  Vulkan-spec fact.
- `libs/ibl/src/CubemapUtils.cpp`/`Cubemap.cpp` (the equirect→cubemap
  conversion's likely direct source) were listed (directory listing)
  but NOT fetched/read in full this session — the equirect-conversion
  row's port-source citation is therefore weaker than the other three
  rows (confirmed to exist and be the right file by name/directory
  co-location with `CubemapIBL.cpp`, not by reading its actual
  conversion math) — flagged as the one row in this matrix that would
  benefit from a follow-up fetch before implementation, budget
  permitting.
