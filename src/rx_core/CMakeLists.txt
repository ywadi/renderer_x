add_library(rx_core STATIC
    src/log.cpp
)
target_include_directories(rx_core PUBLIC include)
target_link_libraries(rx_core PUBLIC spdlog::spdlog glm::glm)

add_executable(rx_core_tests
    tests/doctest_main.cpp
    tests/log_test.cpp
    tests/handle_test.cpp
    tests/math_test.cpp
)
target_link_libraries(rx_core_tests PRIVATE rx_core doctest::doctest)
add_test(NAME rx_core_tests COMMAND rx_core_tests)
