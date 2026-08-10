#pragma once
#include <mutex>

namespace rx::shader::detail {

// The single process-lifetime mutex guarding every call that touches
// Slang's shared global session or anything derived from it -- module
// load/compose/link (Compiler::compileImpl, compiler.cpp) and now also
// reflection's layout walk (reflect(), reflection.cpp).
//
// Why reflection needs it too: slang.h's own doc comment on IGlobalSession
// says front-end operations "still require external synchronization unless
// documented otherwise" [R:A4, R:A6], and `IComponentType::getLayout()` --
// the entry point reflect() calls to get a `ProgramLayout*` -- computes
// (and, on a component type's first call, caches) that layout lazily by
// walking the same session-owned type/layout tables module load and linking
// populate. Nothing in slang.h documents getLayout()/the reflection
// accessor tree as an exception to the general "not thread-safe, externally
// synchronize" rule the way backend codegen
// (getEntryPointCode/getTargetCode/...) is explicitly called out as an
// "experimental" concurrent-safe path once a component type is linked --
// so reflection stays under the same lock as everything else that isn't
// explicitly documented safe.
//
// Declared here (in src/, not include/) rather than as a private detail of
// compiler.cpp alone because it now has exactly two callers across two
// translation units in this library (compiler.cpp, reflection.cpp) that
// must share the SAME mutex object, not one each -- an anonymous-namespace
// function-local static (compiler.cpp's original shape, before this file
// existed) has internal linkage and cannot be referenced from a second TU.
// This header is not installed under rx_shader/include/ and is not part of
// the public API: nothing outside this library's own .cpp files should
// include it.
std::mutex& globalSessionMutex();

}  // namespace rx::shader::detail
