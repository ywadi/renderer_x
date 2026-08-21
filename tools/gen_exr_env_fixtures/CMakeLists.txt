# gen_exr_env_fixtures -- HOST-ONLY tool [Phase 5, issue #75, owner
# insertion into Stage 1 between T10 and T11]: generates
# assets/test/textures/gate_test_env.exr,
# samples/08_gltf_viewer/environments/gate_test_env.exr, and the three
# rejection fixtures (exr_deep_rejected.exr/exr_tiled_rejected.exr/
# exr_dwaa_rejected.exr) by calling the REAL rx::asset::decodeStbImageHdr()
# (the production Radiance-.hdr decode path) and tinyexr's own
# SaveEXRImageToMemory() directly -- see main.cpp's own header comment for
# the full "same generator, second container" provenance rationale. Its
# OUTPUT is committed to the repo (assets/test/textures/,
# samples/08_gltf_viewer/environments/); this tool itself is a fixture-
# regeneration utility, run manually, never part of the normal build or
# test run -- same discipline as tools/gen_gltf_compression_fixtures
# (this directory's own sibling, see its own CMakeLists.txt comment).
#
# Only built for the linux-native preset, same reasoning as
# gen_gltf_compression_fixtures's own CMakeLists.txt (a host-only fixture
# tool never needs to exist as a windows-cross-zig .exe): RX_TARGET_TRIPLE
# (not CMAKE_CROSSCOMPILING -- see that file's own comment on why) is the
# correct guard.
if(RX_TARGET_TRIPLE STREQUAL "x86_64-linux-gnu")
    add_executable(gen_exr_env_fixtures main.cpp)
    target_link_libraries(gen_exr_env_fixtures PRIVATE rx_asset)
    target_include_directories(gen_exr_env_fixtures PRIVATE "${tinyexr_SOURCE_DIR}")
endif()
