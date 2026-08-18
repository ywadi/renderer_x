# gen_gltf_compression_fixtures -- HOST-ONLY tool [Phase 4 Stage 1 Task
# 13]: generates assets/test/cube_meshopt.{gltf,bin} and
# assets/test/cube_draco.{gltf,bin} by calling the REAL meshoptimizer/
# Draco encode APIs directly -- the identical codecs EXT_meshopt_
# compression/KHR_draco_mesh_compression decode consumes, without
# needing gltfpack's own CLI (which additionally pulls in the Basis
# Universal texture encoder purely for KHR_texture_basisu output --
# irrelevant here and an avoidable extra cross-compiled dependency; see
# third_party/CMakeLists.txt's own comment on the meshoptimizer
# vendoring for the full rationale). Its OUTPUT is committed to the repo
# (assets/test/); this tool itself is a fixture-regeneration utility, run
# manually/documented in tools/fetch_assets.sh, never part of the normal
# build or test run.
#
# Only built for the linux-native preset: this tool only ever runs on
# the (Linux) host to regenerate committed fixtures, and its
# windows-cross-zig .exe would need Wine to execute for zero benefit over
# just running the linux-native build of the identical tool. Note
# CMAKE_CROSSCOMPILING is NOT the right guard here -- both this
# project's toolchain files (cmake/toolchains/linux-native.cmake
# included) set CMAKE_SYSTEM_NAME explicitly, which makes CMake report
# CMAKE_CROSSCOMPILING=TRUE even for linux-native (verified directly,
# this task); RX_TARGET_TRIPLE (set by each toolchain file specifically
# so dependents can distinguish them) is the correct check.
if(RX_TARGET_TRIPLE STREQUAL "x86_64-linux-gnu")
    add_executable(gen_gltf_compression_fixtures main.cpp)
    target_link_libraries(gen_gltf_compression_fixtures PRIVATE meshoptimizer::meshoptimizer draco::draco)
endif()
