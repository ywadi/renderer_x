// The sole STB_IMAGE_IMPLEMENTATION translation unit for this entire
// program -- mirrors vma_impl.cpp's own single-TU-implementation
// discipline for the exact same reason: stb_image.h's real implementation
// only gets compiled wherever STB_IMAGE_IMPLEMENTATION is defined before
// the header is included, and defining it in more than one TU is a
// duplicate-symbol link error. Every other consumer of stb_image (none in
// this task; texture.cpp decodes nothing from disk yet -- Uploader::
// uploadToImage() takes already-decoded pixels, per the brief's own
// interface) would only ever #include <stb_image.h> for declarations,
// never redefine STB_IMAGE_IMPLEMENTATION.
//
// Nothing in Task 4 actually calls into stb_image yet -- Texture2D/
// Uploader's own interfaces (per the brief) take raw pixel bytes in, not
// a file path, so there is no in-tree call site for e.g. stbi_load() in
// this task. This TU exists anyway, now, so the dependency (third_party/
// CMakeLists.txt's stb fetch, spec Fixed decision #10) is fully wired
// end to end -- compiled, linked, ready -- for the sample tasks (6+) that
// will actually call stbi_load() to decode a real PNG from disk.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
