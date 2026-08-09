#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>

namespace {

constexpr std::uint32_t kSpirvMagicNumber = 0x07230203;

// SPIR-V's magic number is stored as four bytes at the start of the module,
// least-significant byte first (0x03, 0x02, 0x23, 0x07) regardless of host
// endianness -- compose it explicitly from bytes rather than reinterpreting
// the buffer as a uint32_t, which would only be correct on little-endian
// hosts.
std::uint32_t readMagicNumberLittleEndian(const char* path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());

    unsigned char bytes[4] = {0, 0, 0, 0};
    file.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    REQUIRE(file.gcount() == static_cast<std::streamsize>(sizeof(bytes)));

    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

}  // namespace

TEST_CASE("triangle.vert.spv starts with the SPIR-V magic number") {
    CHECK(readMagicNumberLittleEndian(RX_TRIANGLE_VERT_SPV) == kSpirvMagicNumber);
}

TEST_CASE("triangle.frag.spv starts with the SPIR-V magic number") {
    CHECK(readMagicNumberLittleEndian(RX_TRIANGLE_FRAG_SPV) == kSpirvMagicNumber);
}
