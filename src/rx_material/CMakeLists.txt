# rx_material: materials as Slang modules, pass signatures, and the lazy
# content-hash-keyed pipeline cache [spec Phase 3 design, D6-D8]. Depends on
# rx_graph (PassSignature -- pass_signature.h), rx_shader (ShaderLayoutInfo
# only, not the Compiler class: MaterialSystem drives slang::ISession
# directly -- see material_system.cpp's own header comment for why
# rx_shader::Compiler's public surface is too narrow for the multi-module
# link-time-specialization composition this library needs), and rx_rhi_vk
# (PipelineLayoutBuilder, BindlessTable, Device) -- matching this task's
# architecture diagram [spec Phase 3 design, Architecture].
#
# Links slang::slang directly (like rx_shader does), NOT rx_shader itself --
# there is no rx_shader type anywhere in this library's own headers or
# implementation (rx_shader::Compiler/CompileResult/reflect() are all
# single-module-composition-shaped, per compiler.h/reflection.h's own
# scope, and this library's multi-module (entry-point module + material
# module) composition needs the raw slang::ISession API those wrap).
#
# api_impl.cpp [Task 6, spec Phase 3 design D5]: the COM-lite public ABI
# surface's implementation (rx_api.h's factory + IRxMaterialSystem/
# IRxMaterial/IRxMaterialInstance). Lives in this same static library
# (there is no separate rx.dll yet -- D5 defers the standalone-DLL
# artifact) but never includes material_system.cpp's own internals
# outside its public class -- only material_system.h's public surface.
add_library(rx_material STATIC
    material_system.cpp
    instance.cpp
    api_impl.cpp
)
target_include_directories(rx_material PUBLIC include)
target_link_libraries(rx_material PUBLIC rx_core rx_graph rx_rhi_vk rx_shader_layout_types slang::slang)

# RX_MATERIAL_SHADER_DIR -- the fixed, checked-out-repo-relative location of
# shaders/material/material.slang + forward_entry.slang, baked in at compile
# time exactly like shaders/CMakeLists.txt already bakes
# RX_TRIANGLE_VERT_SPV/RX_TRIANGLE_FRAG_SPV into shader_spirv_test: an
# absolute path anchored at CMAKE_SOURCE_DIR, valid for any build of THIS
# checked-out repository (dev builds, ctest, CI) but not a relocated/
# packaged binary. This remains the DEFAULT MaterialSystem::create() uses
# when its `sharedShaderDir` parameter is left empty (Task 5 through Task 7's
# own unit/GPU tests all rely on exactly that default, unchanged, and keep
# doing so) -- Task 5 deliberately scoped the redistribution case out;
# [Task 8] samples/06_materials is the redistributable-package caller that
# revisits it, passing a runtime-resolved (SDL_GetBasePath()-relative)
# directory instead so its packaged binary needs no reference to this build
# tree at all -- see material_system.h's own comment on
# MaterialSystem::create() for the full parameter contract.
target_compile_definitions(rx_material PRIVATE
  RX_MATERIAL_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/material"
)

# Slang compiles in-process (never statically linked) -- any BINARY linking
# rx_material (transitively, slang::slang) needs the Slang runtime shared
# libraries sitting next to it at load time, exactly like every other
# rx_shader/slang::slang-linking binary in this repo [R:D2]. Not relevant to
# this static library itself, only to executables that link it -- tests/
# CMakeLists.txt calls rx_shader_deploy_runtime_libs() (defined in
# src/rx_shader/CMakeLists.txt, add_subdirectory'd before src/rx_material
# from the root CMakeLists.txt, and visible here per the same cross-
# directory function-name visibility samples/02_hotreload/CMakeLists.txt's
# own comment already documents) directly on its own test binary.
add_subdirectory(tests)
