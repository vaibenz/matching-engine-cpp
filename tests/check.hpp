#pragma once
// Tiny test harness, so the repo builds with nothing but a compiler.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Register {
    Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline int run_all() {
    int failed_cases = 0;
    for (auto& c : registry()) {
        int before = failures();
        c.fn();
        if (failures() != before) {
            ++failed_cases;
            std::printf("FAIL  %s\n", c.name);
        }
    }
    std::printf("%zu cases, %d failed\n", registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                              \
    static void name();                                                         \
    static check::Register CHECK_CAT(reg_, name)(#name, name);                  \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++check::failures();                                                \
            std::printf("  %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        auto va_ = (a);                                                         \
        auto vb_ = (b);                                                         \
        if (!(va_ == vb_)) {                                                    \
            ++check::failures();                                                \
            std::printf("  %s:%d  CHECK_EQ(%s, %s): %s vs %s\n", __FILE__, __LINE__, #a, #b, \
                        std::to_string(va_).c_str(), std::to_string(vb_).c_str()); \
        }                                                                       \
    } while (0)
