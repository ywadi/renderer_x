#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>

// Safe to run in any order relative to device_test.cpp's windowed tests:
// tests/doctest_main.cpp warms vk-bootstrap's process-wide instance-function
// cache with the broadest instance this binary needs before any TEST_CASE
// runs (see the comment there, and the WARNING on Context::create() in
// rx_rhi_vk/context.h, for why that warm-up exists).
TEST_CASE("Context::create succeeds with no required extensions and reports no validation errors") {
    auto ctx = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->instance() != VK_NULL_HANDLE);
    CHECK_FALSE(ctx->hasValidationErrors());
}
