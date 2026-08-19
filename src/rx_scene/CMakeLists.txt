# rx_scene: the engine's no-ECS render-proxy submission layer (Scene,
# spec D19) plus the reversed-Z Camera projection source of truth (spec
# D13) [Phase 4 Stage 2 Task 18, gate matrix-issue05-scene-proxies.md as
# amended by gate/rulings-2026-08-18.md #5 + RC5]. Entirely device-free:
# no VkDevice, no rx_rhi_vk -- Scene's own SoA columns are plain
# std::vectors and Camera's projection helpers are pure GLM math.
#
# Depends on rx_asset PUBLIC: RenderableDesc::mesh is `asset::MeshHandle`
# (the plan's own interface text), and Scene's world-space AABB derivation
# reuses `asset::AABB`/`asset::AABB::transformed()` verbatim (per this
# project's "don't reinvent" rule) rather than re-deriving bounding-box
# transform math. scene.h itself only forward-declares `asset::Registry`
# (meshBoundsFromRegistry()'s production adapter, defined in scene.cpp,
# is the one place that needs the real <rx_asset/registry.h>) -- but the
# PUBLIC link is still against the whole rx_asset target, since
# RenderableDesc's own public fields need MeshHandle/MaterialHandle/AABB
# from mesh_asset.h regardless.
#
# [Phase 4 Stage 2 Task 19] rx_task PUBLIC: DrawListBuilder::build()/
# buildShadow()/recordDrawList() all take `rx::task::Scheduler&` directly
# in their own public signatures (draw_list.h) -- the parallel AABB-cull
# fan-out D15 requires. Still device-free (rx_task itself has no VkDevice
# dependency -- it is a thin enkiTS wrapper, matching this library's own
# "no VkDevice, no rx_rhi_vk" posture verbatim).
add_library(rx_scene STATIC
    camera.cpp
    scene.cpp
    draw_list.cpp
)
target_include_directories(rx_scene PUBLIC include)
target_link_libraries(rx_scene PUBLIC rx_core rx_asset rx_task)

add_subdirectory(tests)
