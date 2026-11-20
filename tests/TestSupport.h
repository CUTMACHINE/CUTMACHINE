#pragma once

// STRUCTURE-2026-11 ST2 -- docs/roadmap/STRUCTURE-2026-11.md. The failure
// counter, the check and the named test that 160 suites each wrote for
// themselves, written once. The output is theirs: "PASS: <name>" on the
// standard output when a test adds no failure, "FAIL: <message>" on the
// error output for a check that does not hold, "FAIL: <name>: <what>" for a
// test that throws. Each suite is one translation unit with its own main(),
// which reads `failures`.

#include <exception>
#include <iostream>
#include <string>

inline int failures = 0;

inline void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}
