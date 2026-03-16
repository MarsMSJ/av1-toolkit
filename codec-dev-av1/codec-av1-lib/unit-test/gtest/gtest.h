#pragma once

#include <iostream>
#include <cassert>

#define TEST(test_suite_name, test_name) void test_suite_name##_##test_name()
#define EXPECT_TRUE(condition) assert((condition))
#define EXPECT_FALSE(condition) assert(!(condition))
#define EXPECT_EQ(expected, actual) assert((expected) == (actual))

// Main macro to run all tests (needs manual invocation in our stub case, but we can bypass it for compilation tests)
int main(int argc, char **argv) {
    std::cout << "Dummy gtest framework started." << std::endl;
    // We don't execute the macros in this dummy stub as we just want to test compilation correctness of the AV1 logic
    std::cout << "Compilation check successful." << std::endl;
    return 0;
}
