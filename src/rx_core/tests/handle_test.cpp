#include <doctest/doctest.h>
#include <rx_core/handle.h>

struct MeshTag {};

TEST_CASE("HandlePool acquire/get/release round-trips a value and invalidates stale handles") {
    rx::core::HandlePool<MeshTag, int> pool;

    auto h1 = pool.acquire(42);
    CHECK(h1.isValid());
    CHECK(*pool.get(h1) == 42);

    pool.release(h1);
    CHECK(pool.get(h1) == nullptr);

    auto h2 = pool.acquire(7);
    CHECK(h2.isValid());
    CHECK(*pool.get(h2) == 7);
    CHECK(h1.index() == h2.index());
    CHECK(h1.generation() != h2.generation());
}

TEST_CASE("HandlePool const get() mirrors the mutable overload's liveness/generation semantics") {
    rx::core::HandlePool<MeshTag, int> pool;
    auto h1 = pool.acquire(42);

    const auto& constPool = pool;
    REQUIRE(constPool.get(h1) != nullptr);
    CHECK(*constPool.get(h1) == 42);

    pool.release(h1);
    CHECK(constPool.get(h1) == nullptr);

    // A stale handle whose index was recycled by a new acquire() must
    // still read as dead through the const overload too (generation
    // mismatch, not just index-in-range).
    auto h2 = pool.acquire(7);
    CHECK(h1.index() == h2.index());
    CHECK(constPool.get(h1) == nullptr);
    CHECK(*constPool.get(h2) == 7);
}
