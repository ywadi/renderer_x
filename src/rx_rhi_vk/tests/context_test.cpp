// This is the only test translation unit in rx_rhi_vk_tests (unlike
// rx_core_tests, which shares a separate tests/doctest_main.cpp across
// several test files), so it both implements doctest's runtime and
// provides main() directly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>

TEST_CASE("Context::create succeeds with no required extensions and reports no validation errors") {
    auto ctx = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->instance() != VK_NULL_HANDLE);
    CHECK_FALSE(ctx->hasValidationErrors());
}
