// shaders/material/energy_compensation_off.slang -- Phase 5 Stage 1 Task 8
// [#44, gate rulings T7/T8]: the OFF resolution of brdf.slang's
// `IEnergyCompensationFeature` link-time composition point, wired into the
// REAL production MaterialSystem pipeline (not just a throwaway test
// composite -- test_brdf_spirv_link_composition_gpu.cpp, T7's own file,
// already proved this exact link-time-composition SHAPE against a
// standalone compute entry point; this file is the production-path half).
//
// MaterialSystem::create() loads this file (and its ON sibling,
// energy_compensation_on.slang) once, alongside forward_entry.slang, via
// the same loadSharedModule() helper [material_system.cpp]. Any material
// module that declares `extern struct EnergyComp : IEnergyCompensationFeature;`
// (standard_pbr.slang does -- see that file's own header comment) gets
// composed against ONE of these two files at link time, selected by
// PipelineRequest::specializationBits' kSpecializationEnergyCompensation
// bit [material_system.h] -- this is D28's fixed-function axis's sibling
// mechanism for the SHADER-RECOMPILE-triggering axis Phase-3 D8 reserved
// `specializationBits` for, made real for the first time by this ticket.
import brdf;

export struct EnergyComp : IEnergyCompensationFeature = EnergyCompensationOff;
