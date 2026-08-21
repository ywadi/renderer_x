# Phase 5 Task 5 -- rx_frame_loop: the shared present-loop helper (ticket
# #41, gate ruling T5). An OPTIONAL engine module, layered exactly like
# rx_debug_ui (src/rx_debug_ui/CMakeLists.txt) -- see present_loop.h's own
# header comment for the full module-home rationale (matrix-p5t05 Open
# Questions #3) and the Main-loop-ownership decision this satisfies
# (docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:
# 500-508). No rx_* CORE library depends on this module; a host embedding
# RendererX that owns its own frame loop never needs to link it.
add_library(rx_frame_loop STATIC
    present_loop.cpp
)
target_include_directories(rx_frame_loop PUBLIC include)
target_link_libraries(rx_frame_loop PUBLIC rx_platform rx_rhi_vk rx_graph rx_core)

add_subdirectory(tests)
