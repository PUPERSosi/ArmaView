#pragma once
#include <filesystem>
#include <string>

// Writes its test files into dir; "n check(s) failed" + one line per check; each failure is also a logged error.
std::string RunCoreTest(const std::filesystem::path& dir, int* failures = nullptr);
