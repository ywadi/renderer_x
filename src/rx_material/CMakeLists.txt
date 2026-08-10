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
add_library(rx_material STATIC
    material_system.cpp
)
target_include_directories(rx_material PUBLIC include)
target_link_libraries(rx_material PUBLIC rx_core rx_graph rx_rhi_vk rx_shader_layout_types slang::slang)

# RX_MATERIAL_SHADER_DIR -- the fixed, checked-out-repo-relative location of
# shaders/material/material.slang + forward_entry.slang, baked in at compile
# time exactly like shaders/CMakeLists.txt already bakes
# RX_TRIANGLE_VERT_SPV/RX_TRIANGLE_FRAG_SPV into shader_spirv_test: an
# absolute path anchored at CMAKE_SOURCE_DIR, valid for any build of THIS
# checked-out repository (dev builds, ctest, CI) but not a relocated/
# packaged binary -- a deliberate, explicitly-scoped-out concern for this
# task (Task 5 is the core material system + its own unit/GPU tests, run
# straight from the build tree against the checked-out source, exactly
# like rx_shader_tests/rx_graph_tests already do; a later sample that ships
# a redistributable package is expected to revisit this the same way
# samples/02_hotreload's own build-relative shader deployment does for its
# single hotreload.slang file) -- see material_system.h's own comment on
# MaterialSystem::create() for the full rationale.
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
