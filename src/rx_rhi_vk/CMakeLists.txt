# volk_SOURCE_DIR/vma_SOURCE_DIR/stb_SOURCE_DIR are populated (and
# PARENT_SCOPE-propagated up to the root) by third_party/CMakeLists.txt's
# FetchContent_Populate calls -- see the comments there for why it's
# Populate (source only) rather than MakeAvailable (which would also
# add_subdirectory each library's own CMakeLists.txt: volk's problematic
# `volk` target, VMA's sample-application target with its unwanted GLFW
# dependency, and stb has no CMakeLists.txt at all to add_subdirectory).
#
# src/vma_impl.cpp is the sole VMA_IMPLEMENTATION translation unit for this
# entire program (see that file); every other consumer of VMA
# (src/buffer.cpp, include/rx_rhi_vk/buffer.h) only ever declares against
# vk_mem_alloc.h, never redefines VMA_IMPLEMENTATION. src/stb_impl.cpp is
# the equivalent sole STB_IMAGE_IMPLEMENTATION translation unit, for the
# identical reason (see that file's own comment).
add_library(rx_rhi_vk STATIC
    src/context.cpp
    src/device.cpp
    src/buffer.cpp
    src/command.cpp
    src/frame_sync.cpp
    src/pipeline_layout.cpp
    src/bindless.cpp
    src/upload.cpp
    src/deletion_queue.cpp
    src/texture.cpp
    src/mesh_buffers.cpp
    src/vma_impl.cpp
    src/stb_impl.cpp
    ${volk_SOURCE_DIR}/volk.c
)
target_include_directories(rx_rhi_vk PUBLIC include ${volk_SOURCE_DIR} ${vma_SOURCE_DIR}/include ${stb_SOURCE_DIR})
# rx_shader_layout_types (defined in src/rx_shader/CMakeLists.txt) is a
# header-only INTERFACE target exposing ONLY rx_shader's plain-data
# shader_layout_info.h -- never `rx_shader` itself, which would drag
# slang::slang (a real link dependency, plus every other rx_shader header)
# into this library. See pipeline_layout.h's own comment for why
# PipelineLayoutBuilder needs `rx::shader::ShaderLayoutInfo` without any of
# that.
target_link_libraries(rx_rhi_vk PUBLIC rx_core vk-bootstrap::vk-bootstrap rx_shader_layout_types)
target_compile_definitions(rx_rhi_vk PUBLIC VK_NO_PROTOTYPES)

# Ten test translation units now (context_test.cpp, device_test.cpp,
# buffer_test.cpp, clear_color_test.cpp, frame_sync_test.cpp,
# pipeline_layout_test.cpp, bindless_test.cpp, upload_test.cpp,
# deletion_queue_test.cpp, texture_test.cpp), so -- like rx_core_tests,
# and unlike this target before device_test.cpp existed -- doctest's
# runtime/main is implemented once in a dedicated tests/doctest_main.cpp
# shared by all ten, rather than in any one test file. doctest_main.cpp
# also links rx_platform: it uses rx::platform::Window to warm up a real
# vk-bootstrap hazard before any TEST_CASE runs (see the comment there) --
# so test order here is not load-bearing, deliberately; no --order-by is
# needed on the add_test line below. clear_color_test.cpp,
# pipeline_layout_test.cpp, bindless_test.cpp, and deletion_queue_test.cpp
# in particular are pure headless (no window/surface/swapchain) and rely
# on that warm-up to be safe regardless of where doctest schedules them
# relative to the windowed tests -- see the Phase 2 plan's Global
# Constraints on the vk-bootstrap landmine: pipeline_layout_test.cpp,
# bindless_test.cpp, and deletion_queue_test.cpp each build their own
# headless device, and joining THIS binary rather than a new one is what
# keeps them protected by the warm-up above instead of needing to
# reimplement it. upload_test.cpp/texture_test.cpp are windowed (Uploader::
# create() needs a real rx::rhi::Device&), skip-guarded exactly like
# buffer_test.cpp/device_test.cpp on a display-less machine.
add_executable(rx_rhi_vk_tests
    tests/doctest_main.cpp
    tests/context_test.cpp
    tests/device_test.cpp
    tests/buffer_test.cpp
    tests/clear_color_test.cpp
    tests/frame_sync_test.cpp
    tests/pipeline_layout_test.cpp
    tests/bindless_test.cpp
    tests/upload_test.cpp
    tests/deletion_queue_test.cpp
    tests/texture_test.cpp
)
target_link_libraries(rx_rhi_vk_tests PRIVATE rx_rhi_vk rx_platform doctest::doctest)
add_test(NAME rx_rhi_vk_tests COMMAND rx_rhi_vk_tests)
