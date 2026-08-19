# samples/common: small, reusable helpers shared across samples --
# currently just D17's tolerance-based reference-PNG gate (reference_gate.h/
# .cpp), first landed by Task 16 for sample 08_gltf_viewer and explicitly
# scoped (spec D17) to be reused, unmodified, by sample 09 later. Not a
# public rx_* library (no install/ABI surface) -- a plain static library
# samples link against directly, matching this project's own "samples are
# leaf consumers, not libraries other rx_* code depends on" posture.
#
# PUBLIC rx_rhi_vk: reference_gate.cpp calls into stb_image.h/
# stb_image_write.h -- the DECLARATIONS only (stb_image.h's own
# STB_IMAGE_IMPLEMENTATION translation unit already exists exactly once, in
# rx_rhi_vk/src/stb_impl.cpp) -- and rx_rhi_vk already exposes
# ${stb_SOURCE_DIR} as a PUBLIC include directory, so linking it here is
# sufficient to find both stb headers with no separate include-path wiring.
# rx_core: RX_LOG_ERROR.
add_library(sample_common STATIC
    reference_gate.cpp
)
target_include_directories(sample_common PUBLIC .)
target_link_libraries(sample_common PUBLIC rx_core rx_rhi_vk)
