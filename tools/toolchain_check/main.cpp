#include <cstdio>

int main() {
#if defined(_WIN32)
    std::puts("renderer_x toolchain_check: target=windows");
#elif defined(__linux__)
    std::puts("renderer_x toolchain_check: target=linux");
#else
    std::puts("renderer_x toolchain_check: target=unknown");
#endif
    return 0;
}
