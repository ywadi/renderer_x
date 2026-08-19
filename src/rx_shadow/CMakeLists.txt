# rx_shadow: the scene shadow quality bridge [spec D21, Phase 4 Stage 2
# Task 22; gate matrix-issue23-shadow-bridge.md as amended by
# gate/rulings-2026-08-18.md's #23 + RC2/RC3]. Two halves:
#   - shadow_frustum.{h,cpp}: device-free fitted-ortho + texel-snapping
#     math (no VkDevice, mirrors rx_scene's own "device-free" posture).
#   - shadow_caster_pipeline.{h,cpp}: the real GPU shadow-caster pipeline,
#     built OUTSIDE rx::material::MaterialSystem entirely (RC3) -- owns
#     its own minimal Slang-compiled shader + pipeline layout via
#     rx_shader::Compiler/reflect + rx_rhi_vk::PipelineLayoutBuilder,
#     exactly like every other runtime-compiled pipeline in this codebase.
add_library(rx_shadow STATIC
    shadow_frustum.cpp
    shadow_caster_pipeline.cpp
)
target_include_directories(rx_shadow PUBLIC include)
target_link_libraries(rx_shadow PUBLIC rx_core rx_rhi_vk rx_shader)

# RX_SHADOW_SHADER_DIR: the directory shadow_caster_pipeline.cpp's
# create() reads shadow_caster.vert.slang from by default -- same
# compile-time-baked-absolute-path idiom rx_material's own
# RX_MATERIAL_SHADER_DIR establishes (material_system.cpp), correct for a
# build-tree run (this library's own tests, unchanged); a redistributed
# binary would need the same `shaderDir` runtime-resolution override
# create() already exposes for exactly that reason (not exercised by this
# library's own tests, which all run from the build tree).
target_compile_definitions(rx_shadow PRIVATE
    RX_SHADOW_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/shadow"
)

add_subdirectory(tests)
