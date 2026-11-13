// ArmaViewFuzz: libFuzzer entry (built with /fsanitize=address,fuzzer by the "fuzz" preset). The first input byte picks
// the parser, the rest is the file. Corpus files start with that byte: see FuzzSeedCorpus (ARMAVIEW_FUZZ_SEEDS).
#include "Fuzz.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1) return 0;
    FuzzOne(FuzzKind(data[0] % uint8_t(FuzzKind::Count)), data + 1, size - 1);
    return 0;
}

// ARMAVIEW_FUZZ_SEEDS="<corpus dir>|file1;file2;...": writes each file with its kind byte in front into the corpus
// folder and exits (the starting corpus for libFuzzer).
extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    const char* env = std::getenv("ARMAVIEW_FUZZ_SEEDS");
    if (!env) return 0;
    std::string s(env);
    const size_t bar = s.find('|');
    if (bar == std::string::npos) return 0;
    const std::filesystem::path dir = s.substr(0, bar);
    std::filesystem::create_directories(dir);
    std::stringstream list(s.substr(bar + 1));
    int n = 0;
    for (std::string f; std::getline(list, f, ';');)
    {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ofstream out(dir / (std::to_string(n++) + "_" + std::filesystem::path(f).filename().string()), std::ios::binary);
        out.put(char(FuzzKindOf(f)));
        out << body;
    }
    std::cout << n << " seed file(s) written to " << dir.string() << "\n";
    std::exit(0);
}
