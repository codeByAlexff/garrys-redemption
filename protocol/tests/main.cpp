#include <cstdio>
#include <cstring>

#include "gr_test.h"

// Usage: gr_tests [substring]   runs the cases whose name contains the substring.
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    for (const auto& c : gr_test::Cases()) {
        if (*filter && !std::strstr(c.name, filter)) continue;
        const int before = gr_test::Failures();
        std::printf("%s\n", c.name);
        std::fflush(stdout);
        c.fn();
        if (gr_test::Failures() == before) std::printf("    ok\n");
        ++ran;
    }
    std::printf("\n%d cases, %d failed checks\n", ran, gr_test::Failures());
    return gr_test::Failures() == 0 ? 0 : 1;
}
