#pragma once

// Minimal gtest-compatible stub.
// No class, no throwing destructor.
// ASSERT_* exits the test function via return (test functions are void).
// EXPECT_* records failure and continues.
// Output format matches real gtest so CI logs are readable.

#include <iostream>
#include <vector>
#include <string>
#include <functional>

// ---------------------------------------------------------------------------
// Failure counter (process-global; compared before/after each test in main)
// ---------------------------------------------------------------------------
inline int& gtest_failure_count() { static int n = 0; return n; }

// ---------------------------------------------------------------------------
// Core helper — records failure and returns whether the check passed.
// ---------------------------------------------------------------------------
inline bool gtest_check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        ++gtest_failure_count();
        std::cerr << file << ":" << line << ": Failure\n  " << expr << "\n";
    }
    return cond;
}

// ---------------------------------------------------------------------------
// EXPECT_* — non-fatal: records failure, test continues
// ---------------------------------------------------------------------------
#define EXPECT_TRUE(c)   gtest_check(!!(c),       "Expected true: "  #c,           __FILE__, __LINE__)
#define EXPECT_FALSE(c)  gtest_check(!(c),        "Expected false: " #c,           __FILE__, __LINE__)
#define EXPECT_EQ(a, b)  gtest_check((a) == (b),  "Expected eq: "    #a " == " #b, __FILE__, __LINE__)
#define EXPECT_NE(a, b)  gtest_check((a) != (b),  "Expected ne: "    #a " != " #b, __FILE__, __LINE__)
#define EXPECT_GE(a, b)  gtest_check((a) >= (b),  "Expected ge: "    #a " >= " #b, __FILE__, __LINE__)
#define EXPECT_LE(a, b)  gtest_check((a) <= (b),  "Expected le: "    #a " <= " #b, __FILE__, __LINE__)
#define EXPECT_GT(a, b)  gtest_check((a) >  (b),  "Expected gt: "    #a " > "  #b, __FILE__, __LINE__)
#define EXPECT_LT(a, b)  gtest_check((a) <  (b),  "Expected lt: "    #a " < "  #b, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// ASSERT_* — fatal: records failure and exits the test function immediately.
// Works because all TEST() bodies are void functions.
// ---------------------------------------------------------------------------
#define ASSERT_TRUE(c)   do { if (!gtest_check(!!(c),      "Assert true: "  #c,           __FILE__, __LINE__)) return; } while(0)
#define ASSERT_FALSE(c)  do { if (!gtest_check(!(c),       "Assert false: " #c,           __FILE__, __LINE__)) return; } while(0)
#define ASSERT_EQ(a, b)  do { if (!gtest_check((a) == (b), "Assert eq: "    #a " == " #b, __FILE__, __LINE__)) return; } while(0)
#define ASSERT_NE(a, b)  do { if (!gtest_check((a) != (b), "Assert ne: "    #a " != " #b, __FILE__, __LINE__)) return; } while(0)

// ---------------------------------------------------------------------------
// Test registration
// ---------------------------------------------------------------------------
struct GTestCase {
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<GTestCase>& gtest_all_cases() {
    static std::vector<GTestCase> v;
    return v;
}

struct GTestRegistrar {
    GTestRegistrar(const char* suite, const char* name, void (*fn)()) {
        gtest_all_cases().push_back({suite, name, fn});
    }
};

#define TEST(suite, name)                                                      \
    static void suite##_##name##_impl();                                       \
    static GTestRegistrar gtest_reg_##suite##_##name(                          \
        #suite, #name, suite##_##name##_impl);                                 \
    static void suite##_##name##_impl()

// ---------------------------------------------------------------------------
// main — runs all registered tests, prints gtest-style summary
// ---------------------------------------------------------------------------
int main(int /*argc*/, char** /*argv*/) {
    int total = 0, passed = 0, failed = 0;
    std::vector<std::string> failed_names;

    for (auto& tc : gtest_all_cases()) {
        ++total;
        std::string full = std::string(tc.suite) + "." + tc.name;
        std::cout << "[ RUN      ] " << full << "\n";

        int before = gtest_failure_count();
        try {
            tc.fn();
        } catch (...) {
            std::cerr << "  Unexpected exception in " << full << "\n";
            ++gtest_failure_count();
        }

        if (gtest_failure_count() == before) {
            std::cout << "[       OK ] " << full << "\n";
            ++passed;
        } else {
            std::cout << "[  FAILED  ] " << full << "\n";
            ++failed;
            failed_names.push_back(full);
        }
    }

    std::cout << "\n[==========] " << total << " test(s) ran.\n"
              << "[  PASSED  ] " << passed << " test(s).\n";
    if (failed > 0) {
        std::cout << "[  FAILED  ] " << failed << " test(s):\n";
        for (auto& n : failed_names)
            std::cout << "    " << n << "\n";
        return 1;
    }
    return 0;
}
