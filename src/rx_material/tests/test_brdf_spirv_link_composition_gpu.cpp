// src/rx_material/tests/test_brdf_spirv_link_composition_gpu.cpp -- Phase 5
// Stage 1 Task 7 [#43, gate ruling T7]: proves the link-time-composition
// mechanism GUARANTEES a feature's code is absent from SPIR-V when unused
// -- verified AT THE SPIR-V LEVEL (spirv-dis), the codebase's own
// established SPIR-V-verification precedent (material_system.cpp/
// context.cpp/device.cpp's own header comments all cite direct spirv-dis
// verification against this project's real compiled output; this is the
// first time that verification is SCRIPTED into an automated test rather
// than performed manually, closing the gate matrix's own "New gaps" entry:
// "No SPIR-V-content inspection tooling ... exists in this repository
// today").
//
// MECHANISM -- brdf.slang's own `IEnergyCompensationFeature` interface +
// `EnergyCompensationOn`/`EnergyCompensationOff` conforming structs (see
// that file's own "Link-time feature composition" header comment), linked
// via forward_entry.slang's ALREADY-VERIFIED extern/export +
// `ISession::createCompositeComponentType()`/`IComponentType::link()`
// contract -- NOT a new technique, the same mechanism this codebase's
// production material path already uses daily. Two SEPARATE composites are
// built from the SAME entry-point module: one linked against
// EnergyCompensationOn, one against EnergyCompensationOff -- producing two
// INDEPENDENT SPIR-V modules.
//
// EVIDENCE -- EnergyCompensationOn::apply() computes `specular *
// energyCompensation(f0, dfgY)`, which contains the ONLY division
// (`1.0 / dfgY`) anywhere in this test's whole compute kernel;
// EnergyCompensationOff::apply() returns `specular` unchanged and never
// even reads `dfgY`. `spirv-dis`'s own disassembly is grepped for
// `OpFDiv` -- the ON variant must contain at least one, the OFF variant
// must contain ZERO. This is a genuinely PRESENT/ABSENT proof of compiled
// code, not a coarser "file size differs" proxy (a size check is also
// asserted, but only as a secondary sanity signal).
//
// DEVICE-FREE -- Slang compilation and `spirv-dis` are both host-side
// processes; no VkDevice/window is created or needed anywhere in this
// file.
#include <doctest/doctest.h>

#include "brdf_test_harness.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace rx::brdf_test;

namespace {

// Shared shape for both link-time-composed variants' compute kernel: reads
// one query (f0, dfgY, specular), applies the LINK-TIME-RESOLVED
// `EnergyFeature`, writes the result's x channel. `EnergyFeature` itself is
// `extern` here -- resolved to a concrete conforming struct only at LINK
// time by whichever impl module this entry module is composed against
// (see compileLinkedFeatureVariant()'s own header comment,
// brdf_test_harness.h).
constexpr const char* kEntrySource = R"(
import brdf;

extern struct EnergyFeature : IEnergyCompensationFeature;

struct QueryGpu { float f0; float dfgY; float specular; };

[[vk::binding(0, 1)]]
StructuredBuffer<QueryGpu> gQuery;

[[vk::binding(1, 1)]]
RWStructuredBuffer<float> gOut;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    QueryGpu q = gQuery[0];
    EnergyFeature feature;
    float3 result = feature.apply(float3(q.specular, q.specular, q.specular), float3(q.f0, q.f0, q.f0), q.dfgY);
    gOut[0] = result.x;
}
)";

constexpr const char* kImplOnSource = R"(
import brdf;
export struct EnergyFeature : IEnergyCompensationFeature = EnergyCompensationOn;
)";

constexpr const char* kImplOffSource = R"(
import brdf;
export struct EnergyFeature : IEnergyCompensationFeature = EnergyCompensationOff;
)";

// Writes `spirv` to a temp .spv file, runs the CMake-located `spirv-dis`
// binary (RX_SPIRV_DIS, found via find_program() at configure time -- see
// src/rx_material/tests/CMakeLists.txt) against it, and returns the
// disassembly text. REQUIREs a clean (0) exit -- a spirv-dis failure means
// the emitted module is not even valid SPIR-V, which is itself a hard
// failure for this test, not a skip.
std::string disassemble(const std::vector<uint32_t>& spirv, const std::filesystem::path& tmpDir, const std::string& tag) {
    auto spvPath = tmpDir / (tag + ".spv");
    auto disPath = tmpDir / (tag + ".dis.txt");

    {
        std::ofstream out(spvPath, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));
    }

    std::string cmd = std::string("\"") + RX_SPIRV_DIS + "\" \"" + spvPath.string() + "\" -o \"" + disPath.string() + "\"";
    int rc = std::system(cmd.c_str());
    REQUIRE_MESSAGE(rc == 0, "spirv-dis invocation failed: ", cmd);

    std::ifstream in(disPath);
    REQUIRE(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t countOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

TEST_CASE("Slang link-time composition: EnergyCompensationOff's compiled SPIR-V contains ZERO OpFDiv instructions "
          "-- EnergyCompensationOn's own division is never generated when the feature is linked out") {
    auto session = createBrdfSession(RX_MATERIAL_SHADER_DIR);
    REQUIRE(session.has_value());

    auto onResult = compileLinkedFeatureVariant(session->session.get(), "RxEnergyFeatureEntryOn", kEntrySource,
                                                 "csMain", "RxEnergyFeatureImplOn", kImplOnSource);
    REQUIRE_MESSAGE(onResult.ok, onResult.diagnostics);
    REQUIRE(onResult.entryPointCode.size() == 1);

    auto offResult = compileLinkedFeatureVariant(session->session.get(), "RxEnergyFeatureEntryOff", kEntrySource,
                                                  "csMain", "RxEnergyFeatureImplOff", kImplOffSource);
    REQUIRE_MESSAGE(offResult.ok, offResult.diagnostics);
    REQUIRE(offResult.entryPointCode.size() == 1);

    const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
    std::string onDis = disassemble(onResult.entryPointCode[0].code, tmpDir, "rx_brdf_energy_feature_on");
    std::string offDis = disassemble(offResult.entryPointCode[0].code, tmpDir, "rx_brdf_energy_feature_off");

    size_t onDivCount = countOccurrences(onDis, "OpFDiv");
    size_t offDivCount = countOccurrences(offDis, "OpFDiv");

    INFO("ON variant OpFDiv count: ", onDivCount);
    INFO("OFF variant OpFDiv count: ", offDivCount);

    // The ON variant's energyCompensation() computes `1.0 / dfgY` -- at
    // least one OpFDiv MUST be present, or this test's own "positive
    // control" is broken (a false pass would otherwise be indistinguishable
    // from a real absence proof).
    CHECK(onDivCount >= 1);
    // The OFF variant's apply() never reads dfgY at all -- ZERO division
    // instructions anywhere in its compiled SPIR-V. This is the load-
    // bearing assertion: the feature's code is provably ABSENT, not merely
    // unreached at runtime.
    CHECK(offDivCount == 0);

    // Secondary sanity signal: the featureless variant's own compiled word
    // count is smaller (a coarser corroborating check, not the proof
    // itself).
    CHECK(offResult.entryPointCode[0].code.size() < onResult.entryPointCode[0].code.size());
}

// Revert-discrimination companion [standing project discipline]: if
// EnergyFeature were resolved to EnergyCompensationOn for BOTH composites
// (a plausible harness bug -- e.g. accidentally reusing the ON impl module
// name/instance for the OFF composite), this test would show onDivCount ==
// offDivCount >= 1 and FAIL its own `offDivCount == 0` assertion above --
// see task-07-report.md for the recorded revert-proof command tail (this
// mutation was run for real against the built test binary, not just
// reasoned about here).
