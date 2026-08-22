### Task 13: Physical light units + punctual lights (KHR_lights_punctual consumption)

Grow `rx_scene` lights to the charter's set with physical units per
Filament's model: directional in lux (exists), point/spot in lumens/candela
with inverse-square attenuation + radius windowing, spot inner/outer cone
semantics; consume KHR_lights_punctual at import (parsed + preserved since
Phase 4 — turns on here, per charter). Pre-exposure/range policy per Task 4's
ruling.

**Files:** `src/rx_scene` (light descs/SoA managers), `src/rx_asset`
(punctual-light consumption into ImportedScene), `shaders/material` direct-
lighting units, tests.
**Acceptance sketch:**
- Unit math matches Filament reference formulas (candela/lumen conversions,
  attenuation window — cited unit tests).
- Authored glTF punctual lights arrive in the Scene with value-asserted
  intensities/cones (decoded-value discipline).
- Single-light analytic falloff probe: rendered intensity at distance d
  matches inverse-square expectation within tolerance.
**Steps:** device-free unit tests → import consumption test → GPU falloff
probe → implement → both presets + real driver → commit.

