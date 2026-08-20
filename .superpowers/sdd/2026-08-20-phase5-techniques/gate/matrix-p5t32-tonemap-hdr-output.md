# Completeness matrix — P5 T32 (issue #68): Tone mapping (AgX/ACES) + FG8 HDR display output

**Plan task:** Task 32, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:850-870`).
**Charter binding:** frame-pipeline slot *"...bloom → tone mapping
(AgX/ACES-class, ties FG8 HDR output) → TAA"*
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:447-451`);
registry FG8, *"HDR display output + swapchain colorspace ladder
(techniques phase, with the post stack)"* (spec:221-223).

**Sources consulted (in-repo, 2026-08-20):**
- `shaders/multipass/tonemap.frag.slang` (full file, 19 lines) — the
  CURRENT "Phase 4 utility tonemap" T32 replaces: plain Reinhard
  (`c/(1+c)`), no exposure application visible in this shader (D22 places
  exposure on the tonemap PASS's push constants, not inside this math —
  not fully re-verified this pass, out of T32's own scope to re-litigate
  Task 4's exposure work).
- `src/rx_rhi_vk/src/context.cpp:215-232` — `Context::create()`'s instance
  extension enablement: `vkb::InstanceBuilder::enable_extension(ext)` for
  every caller-supplied `requiredExtensions` entry is **UNCONDITIONAL**
  ("asking for an extension that turns out unavailable fails `build()`
  outright" — the file's own comment, :259-260). No
  `enable_extension_if_present()`-equivalent exists at the INSTANCE level
  today (that pattern exists only for DEVICE extensions via
  `physResult.value().enable_extension_if_present(...)`, used for
  `VK_EXT_CALIBRATED_TIMESTAMPS`/`VK_EXT_MEMORY_BUDGET`,
  `device.cpp:304,325`) — **this is the load-bearing gap**: FG8's
  swapchain-colorspace extension must NOT be added to the unconditional
  `requiredExtensions` list the way window-system extensions are, or a
  driver/loader lacking it (lavapipe, most likely) hard-fails engine
  startup instead of degrading to SDR (row 3).
- `src/rx_rhi_vk/src/device.cpp:447-463,858-876` — swapchain creation via
  `vkb::SwapchainBuilder`; `swapchainFormat_` stored, but nothing reads or
  requests `VkColorSpaceKHR` today (grep: zero hits for `colorSpace`/
  `VkColorSpaceKHR` anywhere under `src/rx_rhi_vk/`).
- Pinned vk-bootstrap source (`.deps-cache/vk-bootstrap-a64efd1cbd75c026/
  include/VkBootstrap.h:848,922`): `vkb::Swapchain::color_space` field
  (defaults `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`) and
  `SwapchainBuilder::set_desired_format(VkSurfaceFormatKHR)` — **confirmed
  the existing vk-bootstrap version already supports requesting an
  alternate format+colorspace pair; no vk-bootstrap upgrade needed for
  FG8.**
- Pinned Vulkan headers (via the build's own vendored `vulkan_core.h`):
  `VK_EXT_swapchain_colorspace` (instance extension; guard macro present,
  line ~15306-15309) exposing `VK_COLOR_SPACE_HDR10_ST2084_EXT`,
  `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` (scRGB),
  `VK_COLOR_SPACE_BT2020_LINEAR_EXT`, `VK_COLOR_SPACE_HDR10_HLG_EXT`,
  `VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT` (lines ~8846-8859); also
  `VK_EXT_hdr_metadata` (device extension, `VkHdrMetadataEXT` +
  `vkSetHdrMetadataEXT`, lines ~12320-12349) — **a companion extension the
  plan/ticket text never names**, needed to set correct mastering-display
  luminance metadata for genuinely correct HDR10 output (see New gaps).
- Pinned SDL3 (`v3.4.14`, confirmed via `.deps-cache/SDL3-*/lib/pkgconfig/
  sdl3.pc`): `SDL_video.h` ships
  `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`/`SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT`/
  `SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT` (window properties, lines
  ~1631-1633) and the display-level `SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN`
  (~line 684) — SDL3 exposes an HDR-capability QUERY surface already
  vendored at the current pin; no SDL3 upgrade needed for the
  capability-query half of FG8.

**Sources consulted (external, fetched 2026-08-20, `google/filament`,
pinned tag `v1.75.0`, Apache-2.0 confirmed via
`raw.githubusercontent.com/google/filament/main/filament/src/ToneMapper.cpp:1-15`
license header, Copyright 2021 The Android Open Source Project):**
- `filament/src/ToneMapper.cpp` (1052 lines, full file read).
  - `ACES` reference implementation: `aces::ACES(float3 color, float
    brightness)` (line 133), full ACES RRT+ODT chain including the
    "ACES to RGB rendering space" step (line 167) and Filament's own
    documented deviation ("specific to Filament and added only to match
    ACES to our legacy tone mapper... a fit of ACES in Rec.709 but with a
    brightness boost", lines 173-174) — `ACESToneMapper::operator()`
    (line 379, `brightness=1.0f`) and `ACESLegacyToneMapper::operator()`
    (line 391, `brightness=1/0.6f`) are the two shipped ACES-class
    variants.
  - A faster approximation is also present: Narkowicz 2015 "ACES Filmic
    Tone Mapping Curve" (cited by name in-source at lines 404,415) — a
    third, cheaper ACES-class option.
  - `AgxToneMapper::operator()` (lines 691-786): ported from **Blender's
    own AgX implementation** (`AgXInsetMatrix`/`AgXOutsetMatrix`, Rec.2020
    primaries, cited in-source: `github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py`,
    line 697), log2 encoding between `AgxMinEv=-12.47393`/`AgxMaxEv=4.026069`
    (derived from Blender's own LOG2_MIN/MAX/MIDDLE_GRAY constants, cited
    in-source, lines 713-717), a 6th-order polynomial sigmoid contrast
    approximation adapted from `iolite-engine.com/blog_posts/minimal_agx_implementation`
    (cited in-source, line 719), and an `AgxLook` enum (`NONE`/`GOLDEN`/
    `PUNCHY`, ASC-CDL-style slope/power/sat grade, lines 731-762).
  - **Both AgX and every ACES variant live in the SAME Apache-2.0 file, at
    the SAME pinned commit — a single port source covers the entire
    tonemap ladder T32 needs.**
- `filament/src/ColorSpaceUtils.h`/`ColorSpaceUtils.cpp` — referenced by
  `ToneMapper.cpp`'s own `#include` (line 20); not fetched in full this
  pass (color-space conversion helpers the ported math calls into —
  implementer should fetch at port time; flagged, not a blocker).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | AgX tonemapper | Filament `AgxToneMapper::operator()` (`ToneMapper.cpp:764-786`), Apache-2.0, pinned `v1.75.0`, itself ported from Blender's `AgX_LUT_Gen`. | consume-now — primary port source | Verified present, full algorithm read and cited above (inset matrix → log2 encode → polynomial sigmoid → look grade → outset matrix → linearize). | Port-parity unit test: a table of (linear-radiance) → (AgX output) pairs computed from the ported formula, exact-tolerance asserted — same discipline as Task 7's BRDF port-parity tests. |
| 2 | ACES-class tonemapper (default candidate) | Filament `aces::ACES()` (`ToneMapper.cpp:133-195`) full RRT+ODT chain; `ACESToneMapper`/`ACESLegacyToneMapper` (lines 379-397); Narkowicz 2015 fast approximation (lines 404-415) as a cheaper fallback tier. | consume-now — same file, same pin | Verified present, three variants at three cost/accuracy points. | Same port-parity discipline as row 1, against the FULL `aces::ACES()` (not the Narkowicz approximation, unless the spec explicitly picks the cheaper tier for a Deck-floor reason). |
| 3 | Default tonemapper (AgX vs ACES) | N/A — plan/ticket explicitly defers ("default per spec ruling", ticket body). | Task-1-spec decision point (Open Question below) | N/A. | Whichever the spec rules, cite it explicitly in the acceptance criteria with the reasoning (perceptual-highlight-rolloff preference is the usual AgX-over-ACES argument; ACES is the broader cross-industry interchange standard) — not left as a silent default. |
| 4 | SDR path regression: existing gates regenerated once with provenance, byte-stable thereafter | Phase 4's own "reference-vs-ground-truth discipline" precedent (this plan's own Global Constraints section, plan:74-81). | consume-now | N/A — process discipline, not a library question. | Ticket's own text stands. |
| 5 | HDR swapchain colorspace ladder: instance-extension enablement | N/A — internal RHI gap. | consume-now | **Load-bearing gap verified** (Sources, `context.cpp:215-232`): `VK_EXT_swapchain_colorspace` must be requested through an OPTIONAL path (a new `is_extension_available()`-gated instance-extension request), NOT folded into the existing unconditional `requiredExtensions` loop the window-system extensions use — the existing loop hard-fails `Context::create()` if any listed extension is missing, which would break every driver/config lacking this extension (lavapipe, most likely, per the "optional capability with an engineered fallback" principle both the plan and CLAUDE.md require). | A device-free/mocked test asserts `Context::create()` succeeds identically whether or not the extension is available, with a query accessor (e.g. `Context::hdrColorspaceExtensionEnabled()`) reporting which happened — mirrors the exact pattern `matrix-issue27-memory-budget.md` row 2 already established for `VK_EXT_memory_budget`'s own opportunistic enablement. |
| 6 | HDR swapchain colorspace ladder: surface format + colorspace request (scRGB / HDR10) | N/A — direct Vulkan/vk-bootstrap mechanism. | consume-now | Verified: `vkb::SwapchainBuilder::set_desired_format(VkSurfaceFormatKHR{format, colorSpace})` (`VkBootstrap.h:922`) already supports requesting e.g. `{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}` (scRGB) or `{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT}` (HDR10) with a documented `add_fallback_format()` chain back to today's SDR `{..., VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}` — **no vk-bootstrap upgrade required**, this is a call-site change plus format/colorspace-pair selection logic in `device.cpp`'s swapchain-build call sites (`device.cpp:447,858`). | Capability-queried at startup (which format+colorspace pairs the surface actually supports, via `vkGetPhysicalDeviceSurfaceFormatsKHR` — already what `vkb::SwapchainBuilder` walks internally); logged fallback to SDR when none of the HDR pairs are available; zero validation errors on either path. |
| 7 | HDR10 mastering-display metadata (`VK_EXT_hdr_metadata`) | N/A — companion Vulkan extension the plan/ticket text never names. | **new gap — recommend consume-now, folded into T32** | Verified present in the pinned Vulkan headers (`VkHdrMetadataEXT`/`vkSetHdrMetadataEXT`). Without it, an HDR10 (`ST2084`) swapchain has undefined/driver-default mastering luminance, which typically looks visibly wrong (crushed or blown out) on a real HDR display even though validation stays clean — a "zero validation errors" pass with no `vkSetHdrMetadataEXT` call would be a **silent correctness gap the honesty-first mandate exists to catch**, not a real pass. | When the HDR10 colorspace is actually selected, `vkSetHdrMetadataEXT` is called with a documented (even if conservative/placeholder, e.g. Rec.2020 primaries + a stated nits ceiling) `VkHdrMetadataEXT`; the acceptance text should say so explicitly rather than leaving HDR10 selection silently under-specified. |
| 8 | SDL3 HDR capability query (display/window HDR state, SDR white level, HDR headroom) | N/A — direct SDL3 API, already vendored. | consume-now | Verified present at the pinned SDL3 v3.4.14: `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`/`SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT`/`SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT`, `SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN`. | Startup log line reports SDL3's own HDR-enabled/headroom read alongside the Vulkan-side capability query (row 6) — two independent signals (windowing layer + Vulkan surface layer) cross-checked rather than trusting either alone, since a compositor can report HDR-capable at the SDL layer while the Vulkan surface still only offers SDR formats (or vice versa on some Linux compositors). |
| 9 | HDR-display correctness as genuine human-hardware MANUAL_VERIFICATION | Phase 4's own MANUAL_VERIFICATION.md precedent (Steam Deck rows throughout, e.g. `MANUAL_VERIFICATION.md:117-150`). | consume-now | N/A — process precedent, already established and directly reusable. | A new MANUAL_VERIFICATION.md section, same shape as the existing Steam Deck sections ("Last run: not yet performed... this claim is honest until a human runs it"), for HDR-display visual correctness — CI/lavapipe/the dev NVIDIA GPU cannot verify actual HDR display output (no HDR display in the vendor matrix today per CLAUDE.md's own "vendor matrix until expanded" text), so this MUST be a disclosed manual row, never silently assumed passing from validation-clean alone. |

---

## Conflicts

None against the plan/ticket text. Row 7 (`VK_EXT_hdr_metadata`) is a
genuine ADDITION the plan/ticket never named — recorded as a new gap
rather than a conflict, since nothing in the plan contradicts it.

## New gaps

- **`VK_EXT_hdr_metadata`** (row 7): not named anywhere in the plan,
  ticket, or charter text for FG8. Without it, HDR10 output is
  technically "on" (validation-clean, colorspace correctly requested) but
  photometrically undefined on a real display — exactly the class of gap
  the phase's "reference-vs-ground-truth... never certified by
  success-only signals" discipline exists to catch before it ships
  unnoticed. Recommend folding directly into T32 rather than deferring —
  it is a small, same-task addition (one more device-extension enablement
  + one struct + one call), not a separate ticket-worthy scope.

## Open Questions (for the coordinator's binding ruling)

1. **AgX vs ACES as the default tonemapper.** Both are fully available
   from the SAME pinned Filament source at zero extra port cost (row 2).
   **Recommendation: default to AgX** — it is the more actively-favored
   choice in current (2026) real-time PBR engines for its highlight
   rolloff behavior on saturated/bright content (the same reasoning that
   led Blender to adopt it as its own default view transform), and
   Filament ships it as a first-class citizen alongside ACES rather than
   as an afterthought. Ship ACES (full `aces::ACES()`, not the Narkowicz
   approximation) as the selectable alternative — both are ported in the
   same task regardless, so this is a low-cost default choice, not a
   scope decision.
2. **`VK_EXT_hdr_metadata` scope** (row 7/New gaps). Recommendation:
   fold into T32 rather than registry-defer — see New gaps rationale.
