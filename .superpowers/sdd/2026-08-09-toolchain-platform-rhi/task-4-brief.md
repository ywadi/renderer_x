### Task 4: rx_core (logging, handles, math)

**Files:**
- Create: `src/rx_core/CMakeLists.txt`
- Create: `src/rx_core/include/rx_core/log.h`, `src/rx_core/src/log.cpp`
- Create: `src/rx_core/include/rx_core/handle.h`
- Create: `src/rx_core/tests/log_test.cpp`, `src/rx_core/tests/handle_test.cpp`, `src/rx_core/tests/math_test.cpp`
- Modify: `third_party/CMakeLists.txt` (add doctest, GLM)
- Modify: `CMakeLists.txt` (add `src/rx_core`)

**Interfaces:**
- Produces: `rx::core::log::init()`, `RX_LOG_INFO(...)`/`RX_LOG_WARN(...)`/`RX_LOG_ERROR(...)` macros; `rx::core::Handle<Tag>` with `.index()`, `.generation()`, `.isValid()`, and `rx::core::HandlePool<Tag,T>::acquire()/release(Handle<Tag>)/get(Handle<Tag>)`. Target `rx_core` (static library) and `doctest::doctest`, `glm::glm` as reusable third-party targets for later tasks.

- [ ] **Step 1: Add doctest and GLM to third_party**

Append to `third_party/CMakeLists.txt`:
```cmake
include(FetchContent)

set(RX_DOCTEST_TAG "v2.5.3")
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG ${RX_DOCTEST_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

set(RX_GLM_TAG "1.0.3")
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG ${RX_GLM_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)
```

- [ ] **Step 2: Write the failing tests**

`src/rx_core/tests/log_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_core/log.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>

TEST_CASE("log::init is idempotent") {
    rx::core::log::init();
    rx::core::log::init();
    CHECK(true);
}

TEST_CASE("RX_LOG_INFO writes the formatted message through spdlog's default logger") {
    rx::core::log::init();
    auto previousDefault = spdlog::default_logger();

    std::ostringstream capture;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(capture);
    auto testLogger = std::make_shared<spdlog::logger>("test", sink);
    testLogger->set_pattern("%v");
    spdlog::set_default_logger(testLogger);

    RX_LOG_INFO("hello {}", 42);

    spdlog::set_default_logger(previousDefault);
    CHECK(capture.str() == "hello 42\n");
}
```

`src/rx_core/tests/handle_test.cpp`:
```cpp
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
```

`src/rx_core/tests/math_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <glm/glm.hpp>

TEST_CASE("GLM is linked and usable") {
    glm::vec3 a{1.0f, 2.0f, 3.0f};
    glm::vec3 b{4.0f, 5.0f, 6.0f};
    glm::vec3 c = a + b;
    CHECK(c.x == doctest::Approx(5.0f));
    CHECK(c.y == doctest::Approx(7.0f));
    CHECK(c.z == doctest::Approx(9.0f));
}
```

- [ ] **Step 3: Run to verify they fail to compile (headers don't exist yet)**

```bash
cmake --preset linux-native && cmake --build --preset linux-native --target rx_core_tests
```
Expected: FAIL — `rx_core/log.h: No such file or directory` (target doesn't exist yet either; this is expected at this point).

- [ ] **Step 4: Implement rx_core**

`src/rx_core/include/rx_core/log.h`:
```cpp
#pragma once
#include <spdlog/spdlog.h>

namespace rx::core::log {

void init();

}  // namespace rx::core::log

#define RX_LOG_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define RX_LOG_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define RX_LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)
```

`src/rx_core/src/log.cpp`:
```cpp
#include <rx_core/log.h>

namespace rx::core::log {

void init() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    initialized = true;
}

}  // namespace rx::core::log
```

`src/rx_core/include/rx_core/handle.h`:
```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace rx::core {

template <typename Tag>
class Handle {
public:
    Handle() = default;
    Handle(uint32_t index, uint32_t generation) : index_(index), generation_(generation) {}

    uint32_t index() const { return index_; }
    uint32_t generation() const { return generation_; }
    bool isValid() const { return generation_ != 0; }

    bool operator==(const Handle& other) const {
        return index_ == other.index_ && generation_ == other.generation_;
    }

private:
    uint32_t index_ = 0;
    uint32_t generation_ = 0;
};

template <typename Tag, typename T>
class HandlePool {
public:
    Handle<Tag> acquire(T value) {
        if (!freeList_.empty()) {
            uint32_t idx = freeList_.back();
            freeList_.pop_back();
            slots_[idx].value = std::move(value);
            slots_[idx].generation += 1;
            slots_[idx].alive = true;
            return Handle<Tag>(idx, slots_[idx].generation);
        }
        slots_.push_back(Slot{std::move(value), /*generation=*/1, /*alive=*/true});
        return Handle<Tag>(static_cast<uint32_t>(slots_.size() - 1), 1);
    }

    void release(Handle<Tag> handle) {
        if (!isLive(handle)) {
            return;
        }
        slots_[handle.index()].alive = false;
        freeList_.push_back(handle.index());
    }

    T* get(Handle<Tag> handle) {
        if (!isLive(handle)) {
            return nullptr;
        }
        return &slots_[handle.index()].value;
    }

private:
    struct Slot {
        T value;
        uint32_t generation = 0;
        bool alive = false;
    };

    bool isLive(Handle<Tag> handle) const {
        return handle.index() < slots_.size() &&
               slots_[handle.index()].alive &&
               slots_[handle.index()].generation == handle.generation();
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
};

}  // namespace rx::core
```

`src/rx_core/CMakeLists.txt`:
```cmake
add_library(rx_core STATIC
    src/log.cpp
)
target_include_directories(rx_core PUBLIC include)
target_link_libraries(rx_core PUBLIC spdlog::spdlog glm::glm)

add_executable(rx_core_tests
    tests/log_test.cpp
    tests/handle_test.cpp
    tests/math_test.cpp
)
target_link_libraries(rx_core_tests PRIVATE rx_core doctest::doctest)
add_test(NAME rx_core_tests COMMAND rx_core_tests)
```

- [ ] **Step 5: Wire into root CMakeLists.txt**

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(src/rx_core)
```

- [ ] **Step 6: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_core_tests
ctest --preset linux-native -R rx_core_tests --output-on-failure
```
Expected: `100% tests passed, 0 tests failed out of 1`, doctest reports all `CHECK`s passing.

- [ ] **Step 7: Commit**

```bash
git add third_party/CMakeLists.txt src/rx_core/ CMakeLists.txt
git commit -m "Add rx_core: logging, generational handles, GLM math"
```

---

