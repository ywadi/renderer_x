// src/rx_asset/tinyexr_impl.cpp -- [issue #75] the sole
// TINYEXR_IMPLEMENTATION translation unit for this whole codebase --
// mirrors this project's established "define an upstream single-header
// library's *_IMPLEMENTATION macro in exactly one .cpp, matching the
// header's own documented contract" discipline (stb_image_resize_impl.cpp/
// rx_rhi_vk/src/stb_impl.cpp/vma_impl.cpp's own identical precedent).
// Defining TINYEXR_IMPLEMENTATION in more than one TU is a duplicate-
// symbol link error, exactly like those three. Lives in rx_asset (the
// actual EXR-decode consumer, texture_decode.cpp) rather than rx_rhi_vk's
// existing stb_impl.cpp, mirroring stb_image_resize_impl.cpp's own
// "implementation TU lives next to its real consumer" choice.
//
// TINYEXR_USE_MINIZ is left at tinyexr.h's own header-default (ON, 1):
// see third_party/CMakeLists.txt's own tinyexr vendoring comment for the
// full rationale (this project compiles tinyexr's vendored deps/miniz/
// miniz.c, see this file's own sibling entry in rx_asset's
// CMakeLists.txt, rather than linking a system zlib or pulling in
// stb_image_write.h's encode-only zlib compressor for a decode-only
// need). No other TINYEXR_USE_* macro is touched -- every other default
// (TINYEXR_USE_PIZ=1, TINYEXR_USE_ZFP=0, TINYEXR_USE_THREAD=0,
// TINYEXR_USE_STB_ZLIB=0) is exactly what this task's own probed
// supported envelope assumes (see texture_decode.h's own EXR block
// comment and task-exr-report.md for the evidence).
#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>
