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
