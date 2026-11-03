#pragma once

#include <exception>
#include <initializer_list>
#include <iostream>
#include <utility>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
        return false; \
    } \
} while (false)

using TestFunction = bool(*)();

inline int run_tests(std::initializer_list<std::pair<const char*, TestFunction>> tests) {
    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            if (!test()) {
                std::cerr << "[FAIL] " << name << '\n';
                passed = false;
            } else {
                std::cout << "[PASS] " << name << '\n';
            }
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": unexpected exception: " << error.what() << '\n';
            passed = false;
        } catch (...) {
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
