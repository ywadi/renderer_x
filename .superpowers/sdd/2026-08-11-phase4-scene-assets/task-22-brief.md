### Task 22: Shadow quality bridge (D21)

**Files:** Modify `shaders/multipass/` shadow path shared pieces as needed → but primary target is the Stage-2 scene shadow path: light ortho fitted to visible bounds (from DrawListBuilder), slope-scaled depth bias (vkCmdSetDepthBias on the shadow pass), 3×3 PCF in the standard lit path (`shaders/material/forward_entry.slang` shadow helper upgrade; sample 05 keeps its own simpler shaders untouched — documented). Reversed-Z main-camera migration lands here for the scene path (clear values, compare ops via PassSignature/pipeline state).
**Steps:** GPU test: acne scene (large ground plane at grazing light) renders without acne (probe variance check) and without peter-panning (contact probe); PCF softness probe (edge gradient spans ≥2 texels) → implement → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue23-shadow-bridge.md` as amended by
`gate/rulings-2026-08-18.md` §#23 + RC2/RC3. Key deltas: FILE LIST
GROWS — `src/rx_graph/{resources.h,executor.cpp}` (D29: AttachmentDesc
`DepthConvention` + per-pass clear values at both executor sites; the
two-convention one-frame test); the shadow pass STAYS standard-Z per
D13, so the depth-bias sign does NOT flip — a required code comment
prevents the plausible-but-wrong "reversed-Z fix" (main-camera
migration lands in the same task); shadow-caster pipeline is built
OUTSIDE MaterialSystem (RC3 option (a) — getPipeline's compare-op
flips as one literal for the main camera; no compare-op axis);
`VK_DYNAMIC_STATE_DEPTH_BIAS` added to the shadow pipeline's dynamic
state (the creation-time detail the ticket omitted);
`depthClampEnable=VK_TRUE` on casters + device-feature check;
comparison-sampler PCF (compareEnable=TRUE, COMPARE_OP_LESS,
SampleCmp-equivalent taps — hardware filtering, not sample 05's manual
compare); texel snapping in scope (two-camera-position shimmer test);
shadow vertex shader uses SV_VulkanInstanceID bindless addressing
(never a push-constant transformIndex); 1024/D32_SFLOAT default kept
but parameterized; acne probe = neighborhood VARIANCE check at ≥80°
grazing, peter-panning probe = caster-base/shadow-edge continuity
within pixel tolerance.

