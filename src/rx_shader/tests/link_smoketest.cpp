// Windows link smoke test (task-1-brief.md step 2; [R:A6, R:D2]): proves
// that a trivial translation unit calling slang::createGlobalSession
// cross-compiles AND links, via the windows-cross-zig preset's zig/LLD
// toolchain, against the MSVC-built slang-compiler.lib import library --
// the one real unknown this task needed to de-risk before writing any of
// rx_shader's real Compiler implementation on top of it (a plan-level risk
// gate: a link failure here means STOP, not improvise). This file is
// deliberately minimal ("a trivial TU") and has no dependency on the rest
// of rx_shader.
//
// Built for both presets (harmless and cheap on linux-native, where the
// equivalent same-ABI link was already known-safe); the actual risk this
// exists to cover is specific to windows-cross-zig.
#include <slang-com-ptr.h>
#include <slang.h>

int main() {
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    SlangResult result = slang::createGlobalSession(globalSession.writeRef());
    return SLANG_FAILED(result) ? 1 : 0;
}
