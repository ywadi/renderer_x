# rx_ibl: the compute IBL bake chain [Phase 5 Stage 1 Task 9, #45] --
# equirect/cube HDR environment -> base cubemap -> diffuse irradiance
# cubemap + prefiltered specular cubemap (roughness-indexed mips) + DFG
# LUT, driven entirely through rx_graph's compute-class render-graph API
# (Task 2) via rx_rhi_vk::ComputePipelineCache. See include/rx_ibl/bake.h
# for the full module-ownership rationale (new, self-contained library --
# no rx_scene/rx_material dependency).
add_library(rx_ibl STATIC
    src/bake.cpp
)
target_include_directories(rx_ibl PUBLIC include)
target_link_libraries(rx_ibl PUBLIC rx_core rx_graph rx_rhi_vk rx_shader rx_task)

# NOTE: unlike rx_material's MaterialSystem::create() (which defaults its
# own `sharedShaderDir` parameter internally), rx_ibl::bakeEnvironment()
# takes `shaderDir` as a REQUIRED, EXPLICIT parameter -- this library
# itself has no compiled-in default shader path at all (nothing in
# bake.cpp reads RX_IBL_SHADER_DIR); RX_IBL_SHADER_DIR is defined below,
# on the TEST executable only, exactly where it is actually consumed.

add_subdirectory(tests)
