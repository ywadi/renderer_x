# Compiles the triangle demo shaders from Slang source to SPIR-V using the
# prebuilt slangc fetched by tools/fetch_slang.cmake (RX_SLANGC), which is
# include()'d from the root CMakeLists.txt before this subdirectory is
# added.

set(RX_TRIANGLE_VERT_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/triangle.vert.slang")
set(RX_TRIANGLE_FRAG_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/triangle.frag.slang")

set(RX_TRIANGLE_VERT_SPV "${CMAKE_CURRENT_BINARY_DIR}/triangle.vert.spv" CACHE INTERNAL "Absolute path to the compiled triangle vertex shader SPIR-V")
set(RX_TRIANGLE_FRAG_SPV "${CMAKE_CURRENT_BINARY_DIR}/triangle.frag.spv" CACHE INTERNAL "Absolute path to the compiled triangle fragment shader SPIR-V")

# Flags verified directly against the pinned slangc 2026.14.1 (`slangc -h`)
# and by running the exact invocation below against both source files: all
# four flags are accepted as-is on this release, no drift found against the
# brief's originally proposed flags.
add_custom_command(
  OUTPUT "${RX_TRIANGLE_VERT_SPV}"
  COMMAND "${RX_SLANGC}" "${RX_TRIANGLE_VERT_SLANG}" -target spirv -profile sm_6_0 -entry main -o "${RX_TRIANGLE_VERT_SPV}"
  DEPENDS "${RX_TRIANGLE_VERT_SLANG}"
  COMMENT "slangc: triangle.vert.slang -> triangle.vert.spv"
  VERBATIM
)

add_custom_command(
  OUTPUT "${RX_TRIANGLE_FRAG_SPV}"
  COMMAND "${RX_SLANGC}" "${RX_TRIANGLE_FRAG_SLANG}" -target spirv -profile sm_6_0 -entry main -o "${RX_TRIANGLE_FRAG_SPV}"
  DEPENDS "${RX_TRIANGLE_FRAG_SLANG}"
  COMMENT "slangc: triangle.frag.slang -> triangle.frag.spv"
  VERBATIM
)

add_custom_target(triangle_shaders ALL
  DEPENDS "${RX_TRIANGLE_VERT_SPV}" "${RX_TRIANGLE_FRAG_SPV}"
)

add_executable(shader_spirv_test tests/spirv_validity_test.cpp)
target_link_libraries(shader_spirv_test PRIVATE doctest::doctest)
target_compile_definitions(shader_spirv_test PRIVATE
  RX_TRIANGLE_VERT_SPV="${RX_TRIANGLE_VERT_SPV}"
  RX_TRIANGLE_FRAG_SPV="${RX_TRIANGLE_FRAG_SPV}"
)
add_dependencies(shader_spirv_test triangle_shaders)
add_test(NAME shader_spirv_test COMMAND shader_spirv_test)
