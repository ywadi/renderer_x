# compare_rgba8_png -- host tool, see main.cpp's own header comment.
# Links sample_common directly (samples/common/CMakeLists.txt) for
# loadRgba8Png()/compareToReference() -- no rx_* core library depends on
# this or vice versa; a plain leaf host-side diagnostic, same posture as
# tools/gen_gltf_compression_fixtures. Builds on both presets (no
# platform-specific code at all, unlike gen_gltf_compression_fixtures'
# Draco-encoder host-only restriction) -- it is a bash-invoked developer/
# verification tool either way.
add_executable(compare_rgba8_png main.cpp)
target_link_libraries(compare_rgba8_png PRIVATE sample_common)
