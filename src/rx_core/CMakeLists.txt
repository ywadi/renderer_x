add_library(rx_core STATIC
    src/log.cpp
)
target_include_directories(rx_core PUBLIC include)
target_link_libraries(rx_core PUBLIC spdlog::spdlog glm::glm)

# Tracy client integration [spec D3, Phase 4 Stage 0 Task 3] -- rx_core is
# the ONE place this project defines TRACY_ENABLE and links Tracy::TracyClient,
# both PUBLIC: rx_core is a public, transitive dependency of every other
# library and sample in this engine, so this single wiring point is what
# makes rx_core/profile.h's RX_ZONE*/RX_FRAME_MARK/RX_PLOT macros (and
# rx_rhi_vk/tracy_gpu.h's own GPU-zone macros, which key off the identical
# TRACY_ENABLE definition) agree, project-wide, on whether Tracy is compiled
# in -- with zero additional per-library CMake wiring. Guarded on RX_TRACY
# (third_party/CMakeLists.txt's own top-level option): when it is OFF, the
# Tracy::TracyClient target does not exist at all (that whole dependency
# block never runs), so neither this compile definition nor this link
# dependency exists either -- the OFF configuration compiles and links with
# zero Tracy symbols anywhere in the resulting binaries, by construction,
# not by convention.
if(RX_TRACY)
    target_compile_definitions(rx_core PUBLIC TRACY_ENABLE)
    target_link_libraries(rx_core PUBLIC Tracy::TracyClient)
endif()

add_executable(rx_core_tests
    tests/doctest_main.cpp
    tests/log_test.cpp
    tests/handle_test.cpp
    tests/math_test.cpp
    tests/profile_test.cpp
)
target_link_libraries(rx_core_tests PRIVATE rx_core doctest::doctest)
add_test(NAME rx_core_tests COMMAND rx_core_tests)
