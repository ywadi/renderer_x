// shaders/material/energy_compensation_on.slang -- Phase 5 Stage 1 Task 8
// [#44]: the ON resolution of brdf.slang's `IEnergyCompensationFeature`
// link-time composition point -- see energy_compensation_off.slang's own
// header comment (this file's sibling) for the full mechanism this pair
// implements. Selected when PipelineRequest::specializationBits carries
// kSpecializationEnergyCompensation [material_system.h].
//
// NOT wired to any shipped default material instance yet [gate matrix
// "Feature-permutation mechanism" row]: `StandardPbrParams::dfgY`
// (standard_pbr.slang) is a real, reflected, per-instance parameter this
// variant's compiled code genuinely reads (proving the mechanism end to
// end, value-asserted by test_standard_pbr_energy_compensation_gpu.cpp),
// but every producer of a StandardPbr instance today (samples 08/09) binds
// it to 1.0 -- the honest "no unaccounted multi-scattering loss assumed"
// neutral value -- because the REAL per-pixel source (a baked DFG/
// environment-BRDF LUT, Filament's own `pixel.dfg.y`) is Task 9's
// deliverable (IBL bake chain), not built yet. This file exists, compiles,
// links, and is numerically correct against an injected dfgY today; wiring
// a real per-pixel dfgY through to shipped content is Task 9/10's job.
import brdf;

export struct EnergyComp : IEnergyCompensationFeature = EnergyCompensationOn;
