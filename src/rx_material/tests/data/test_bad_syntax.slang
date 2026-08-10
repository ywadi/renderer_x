// Deliberately broken module for test_material_system.cpp's "bad-module"
// case: MaterialSystem::loadMaterial() must throw, with the thrown
// message containing Slang's own diagnostic text, rather than crash or
// silently return an unusable handle.
import material;

struct BrokenParams {
    float4 tint
};

[[vk::binding(0, 1)]]
ParameterBlock<BrokenParams> gParams;

struct Broken : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        return gParams.tint;
    }
};

export struct MaterialImpl : IMaterialShader = Broken;
