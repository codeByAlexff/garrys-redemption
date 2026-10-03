// Minimal test harness, so the tests have no dependency to download.

#pragma once

#include <cmath>
#include <cstdio>
#include <vector>

namespace gr_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Cases() {
    static std::vector<Case> cases;
    return cases;
}

inline int& Failures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }
};

inline void Fail(const char* file, int line, const char* what) {
    std::printf("    FAIL %s:%d  %s\n", file, line, what);
    ++Failures();
}

}  // namespace gr_test

#define GR_TEST(name)                                         \
    static void name();                                       \
    static ::gr_test::Registrar registrar_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) ::gr_test::Fail(__FILE__, __LINE__, #cond);      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                          \
    do {                                                                               \
        const double gr_a = static_cast<double>(a);                                    \
        const double gr_b = static_cast<double>(b);                                    \
        if (!(std::fabs(gr_a - gr_b) <= (eps))) {                                      \
            char gr_msg[256];                                                          \
            std::snprintf(gr_msg, sizeof(gr_msg), "%s ~= %s  (%.9g vs %.9g)", #a, #b, gr_a, gr_b); \
            ::gr_test::Fail(__FILE__, __LINE__, gr_msg);                               \
        }                                                                              \
    } while (0)
