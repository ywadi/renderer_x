#include <doctest/doctest.h>
#include <rx_shader/compiler.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;

// A complete, real vertex+fragment module compiled from a single source
// string -- deliberately mirrors shaders/triangle.vert.slang +
// triangle.frag.slang's fullscreen-triangle-without-a-vertex-buffer trick,
// but as two named entry points in one module so a single
// compileFromSource call exercises both stages at once.
const char* kGoodSource = R"(
struct VSOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VSOut vsMain(uint vertexID : SV_VertexID)
{
    float2 positions[3] = float2[3](
        float2(0.0, -0.5),
        float2(0.5, 0.5),
        float2(-0.5, 0.5)
    );
    VSOut o;
    o.position = float4(positions[vertexID], 0.0, 1.0);
    o.color = float4(1.0, 1.0, 1.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fsMain(float4 color : COLOR0) : SV_Target
{
    return color;
}
)";

// Deliberately references an identifier that doesn't exist anywhere in
// scope, so this must fail to compile.
const char* kBadSource = R"(
[shader("fragment")]
float4 fsMain(float4 color : COLOR0) : SV_Target
{
    return colorTypoNotDefined;
}
)";

}  // namespace

TEST_CASE("Compiler::create succeeds and yields a usable compiler") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
}

TEST_CASE("compileFromSource on a known-good shader yields 2 valid SPIR-V blobs with correct stages") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult result =
        compiler->compileFromSource("GoodModule", kGoodSource, {"vsMain", "fsMain"});

    INFO("diagnostics: " << result.diagnostics);
    CHECK(result.ok);
    REQUIRE(result.entryPointCode.size() == 2);

    const rx::shader::SpirvBlob& vs = result.entryPointCode[0];
    CHECK(vs.entryPointName == "vsMain");
    CHECK(vs.stage == VK_SHADER_STAGE_VERTEX_BIT);
    REQUIRE(vs.code.size() >= 1);
    CHECK(vs.code[0] == kSpirvMagic);

    const rx::shader::SpirvBlob& fs = result.entryPointCode[1];
    CHECK(fs.entryPointName == "fsMain");
    CHECK(fs.stage == VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE(fs.code.size() >= 1);
    CHECK(fs.code[0] == kSpirvMagic);

    // The linked component type must be retained for Task 2's reflection.
    CHECK(result.linkedProgram.get() != nullptr);
}

TEST_CASE("compileFromSource on a broken shader fails with diagnostics containing the error line") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult result = compiler->compileFromSource("BadModule", kBadSource, {"fsMain"});

    CHECK_FALSE(result.ok);
    CHECK(result.entryPointCode.empty());
    REQUIRE_FALSE(result.diagnostics.empty());
    // The offending source line is reproduced verbatim in Slang's
    // diagnostic text (source-context display), so this is a direct check
    // that the actual error line made it into the captured diagnostics,
    // not just some generic failure message.
    CHECK(result.diagnostics.find("colorTypoNotDefined") != std::string::npos);
    CHECK(result.linkedProgram.get() == nullptr);
}

TEST_CASE("a second compile through the same Compiler reuses the global session without crashing") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    auto start = std::chrono::steady_clock::now();
    rx::shader::CompileResult first = compiler->compileFromSource("GoodModule", kGoodSource, {"vsMain", "fsMain"});
    auto afterFirst = std::chrono::steady_clock::now();
    rx::shader::CompileResult second = compiler->compileFromSource("GoodModule", kGoodSource, {"vsMain", "fsMain"});
    auto afterSecond = std::chrono::steady_clock::now();

    CHECK(first.ok);
    CHECK(second.ok);
    REQUIRE(second.entryPointCode.size() == 2);
    CHECK(second.entryPointCode[0].code[0] == kSpirvMagic);
    CHECK(second.entryPointCode[1].code[0] == kSpirvMagic);

    // "Sane timing" (not a strict comparative benchmark, which would be
    // flaky under a loaded CI runner): each individual compile call --
    // module load + entry point lookup + compose + link + codegen for a
    // two-entry-point module this small -- should comfortably finish well
    // under a second once the (one-time, process-wide) Slang stdlib load
    // has already happened in Compiler::create() above. A multi-second
    // stall here would indicate the global session is being recreated (or
    // the mutex is somehow serializing against unrelated work) rather
    // than genuinely reused.
    auto firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterFirst - start).count();
    auto secondMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterSecond - afterFirst).count();
    CHECK(firstMs < 5000);
    CHECK(secondMs < 5000);
}

TEST_CASE("creating a second, independent Compiler also reuses the process-wide global session") {
    auto first = rx::shader::Compiler::create();
    REQUIRE(first.has_value());

    auto start = std::chrono::steady_clock::now();
    auto second = rx::shader::Compiler::create();
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(second.has_value());

    // By the time this test runs, some earlier test in this binary has
    // already paid the one-time global-session/stdlib-load cost, so this
    // second (in-process) Compiler::create() should be cheap -- it only
    // needs to create a new (lightweight) ISession, not a new
    // IGlobalSession. A generous bound, not a tight benchmark.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);

    rx::shader::CompileResult result = second->compileFromSource("GoodModule", kGoodSource, {"vsMain"});
    CHECK(result.ok);
}

TEST_CASE("compileFromFile reads and compiles Slang source from disk") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    std::filesystem::path tempPath =
        std::filesystem::temp_directory_path() / "rx_shader_compiler_test_from_file.slang";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << kGoodSource;
    }

    rx::shader::CompileResult result = compiler->compileFromFile(tempPath.string(), {"vsMain", "fsMain"});
    std::filesystem::remove(tempPath);

    INFO("diagnostics: " << result.diagnostics);
    CHECK(result.ok);
    REQUIRE(result.entryPointCode.size() == 2);
    CHECK(result.entryPointCode[0].code[0] == kSpirvMagic);
    CHECK(result.entryPointCode[1].code[0] == kSpirvMagic);
}

TEST_CASE("compileFromFile on a nonexistent path fails cleanly with diagnostics") {
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());

    rx::shader::CompileResult result = compiler->compileFromFile("/nonexistent/path/does_not_exist.slang", {"main"});
    CHECK_FALSE(result.ok);
    REQUIRE_FALSE(result.diagnostics.empty());
}
