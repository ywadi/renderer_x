#pragma once
#include <slang-com-ptr.h>
#include <slang.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rx::shader {

// One SPIR-V module's worth of compiled code for a single entry point,
// ready to hand straight to vkCreateShaderModule.
struct SpirvBlob {
    std::vector<uint32_t> code;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL;
    std::string entryPointName;
};

// Result of one Compiler::compileFromSource/compileFromFile call.
//
// Diagnostics (warnings AND errors -- Slang surfaces both the same way, as
// non-empty IBlob text at every compile step [R:A2]) are always captured
// here, never silently swallowed, regardless of `ok`: a successful compile
// can still carry warning text worth surfacing to a caller/log.
struct CompileResult {
    bool ok = false;
    std::vector<SpirvBlob> entryPointCode;
    std::string diagnostics;

    // Opaque retained handle to the fully linked component type that
    // `entryPointCode` above was extracted from. Kept alive here (rather
    // than released at the end of the compile call) purely so Task 2's
    // reflection code can call `linkedProgram->getLayout(...)` on it
    // later -- the `ProgramLayout*` that call returns is only valid for as
    // long as the IComponentType it came from is alive, and this is that
    // lifetime anchor. Null when `ok` is false.
    Slang::ComPtr<slang::IComponentType> linkedProgram;
};

// Compiles Slang source (from a string or a file) to SPIR-V, in-process,
// via the Slang runtime library (`slang::slang` -- never `slang::gfx`,
// which is deprecated upstream in favor of slang-rhi [R:E3]).
//
// Every Compiler instance shares one process-lifetime `slang::IGlobalSession`
// behind a mutex: per slang.h's own doc comment on IGlobalSession, "a
// single global session object is currently *not* thread-safe... global
// session and the objects created from it should be externally
// synchronized when shared across threads" [R:A4, R:A6]. This class
// deliberately uses exactly one such session for the whole process (never
// one per Compiler) to amortize Slang's real cost -- loading its standard
// library -- across every Compiler instance and every compile call; the
// mutex serializes the front-end operations (module load, composition,
// linking) that touch it. Each Compiler owns its own `slang::ISession`
// (cheap; "session per target config" [spec Fixed decision #2]), created
// once in `create()` and reused for every subsequent compile call on that
// instance -- this is the "session reuse" a hot-reload loop relies on.
class Compiler {
public:
    // Slang::ComPtr already handles move/release correctly on its own, so
    // these can all be defaulted -- there is no raw resource here that
    // needs custom teardown logic.
    Compiler(Compiler&&) noexcept = default;
    Compiler& operator=(Compiler&&) noexcept = default;
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;
    ~Compiler() = default;

    // Creates (or, after the first successful call anywhere in the
    // process, reuses) the shared global session, then creates a fresh
    // SPIR-V-target `ISession` for this Compiler instance -- pinned to an
    // explicit SPIR-V 1.3 capability floor matching the Vulkan 1.3 /
    // Steam Deck baseline [R:A5, spec Fixed decision #3]. Returns
    // std::nullopt (logging the reason via RX_LOG_ERROR) if either step
    // fails.
    //
    // MATRIX LAYOUT -- read before writing any shader that reads a matrix
    // out of a buffer: this session is built via the plain C++ API
    // (`slang::SessionDesc`), which leaves `SessionDesc::
    // defaultMatrixLayoutMode` at its own default -- ROW-major. That is a
    // DIFFERENT default than the `slangc` CLI's own long-standing legacy
    // default (column-major) -- i.e. a shader compiled through THIS
    // `Compiler` and the exact same shader compiled offline via `slangc`
    // (01_triangle's build-time path, shaders/CMakeLists.txt) can lay out
    // an unqualified `float4x4` differently in memory, with no
    // `row_major`/`column_major` qualifier anywhere in the source to flag
    // it. Concretely: a `float4x4` read out of a
    // `ConstantBuffer<T>`/`StructuredBuffer<T>`/push-constant element
    // compiled through this Compiler is ROW-major in memory. Any
    // host-side matrix producer that defaults to COLUMN-major storage
    // instead -- GLM's `glm::mat4`, most notably, since this engine
    // already depends on it (`rx_core` links `glm::glm` PUBLIC) -- must
    // transpose at the host/device boundary before writing those bytes
    // into a buffer a shader compiled through this Compiler will read as
    // a matrix, or the shader reads a transposed (garbled) transform.
    // This was found empirically, not from documentation alone: see
    // `samples/03_bindless_mesh/main.cpp`'s `updateTransforms()` for the
    // worked example (`glm::transpose(viewProj * model)` immediately
    // before the upload) and its comment for the debugging trail that
    // found it (a rendered-and-inspected image, not just reasoning about
    // the spec). `rx::shader::ShaderLayoutInfo` (shader_layout_info.h)
    // reports set/binding/type/count/range shape only -- it carries no
    // matrix-layout information at all, so this doc comment is the one
    // place that contract is written down; see this same paragraph if you
    // land there first.
    static std::optional<Compiler> create();

    // Compiles `source` (a complete Slang translation unit) as a module
    // named `moduleName`, then finds and links every name in
    // `entryPointNames`, in order -- `CompileResult::entryPointCode` is
    // populated in the same order on success.
    CompileResult compileFromSource(const std::string& moduleName,
                                     const std::string& source,
                                     const std::vector<std::string>& entryPointNames);

    // Reads the Slang source at `path` from disk and compiles it exactly
    // like compileFromSource, using the file's stem as the module name so
    // diagnostics reference the real path on disk (useful for hot-reload:
    // error messages point at the file the user is actually editing).
    CompileResult compileFromFile(const std::string& path, const std::vector<std::string>& entryPointNames);

private:
    explicit Compiler(Slang::ComPtr<slang::ISession> session);

    Slang::ComPtr<slang::ISession> session_;
};

}  // namespace rx::shader
