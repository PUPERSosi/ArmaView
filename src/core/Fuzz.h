#pragma once
// Fuzzing of the file parsers: one entry per format (the bytes of a whole file), used by
//  - the ArmaViewFuzz target (libFuzzer + AddressSanitizer, preset "fuzz": src/FuzzMain.cpp), and
//  - a short deterministic run in the selftest (FuzzRun: seed files mutated by bit flips, byte runs, truncation ...).
// A parser must never crash, hang or read out of bounds on any input; errors are fine.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

enum class FuzzKind { P3D, RTM, PAA, TGA, ConfigText, ConfigRap, Rvmat, Count };
const char* FuzzKindName(FuzzKind k);
FuzzKind FuzzKindOf(const std::filesystem::path& file); // by extension / signature

// One parse of `data` as `kind` (parsers that read files get a temp file). Never throws.
void FuzzOne(FuzzKind kind, const uint8_t* data, size_t size);

// Deterministic mutation run: every seed file, `iterations` mutants each. Returns a one-line summary.
// progress: called after each mutant (a long run on the UI thread keeps the hang watchdog quiet)
std::string FuzzRun(const std::vector<std::filesystem::path>& seeds, int iterations, uint32_t randomSeed, int* exceptions = nullptr,
                    const std::function<void()>& progress = {});
