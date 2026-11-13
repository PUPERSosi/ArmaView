#pragma once
#include <string>

struct IDXGIAdapter;
std::string MemoryReport(IDXGIAdapter* adapter); // RAM working set / private, VRAM usage
