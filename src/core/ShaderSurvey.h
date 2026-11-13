#pragma once
// Which PixelShaderIDs the RVMATs on the work drive use (a3 and mods), counted on a worker, each with how far the
// viewer renders it (implemented / approximated / not implemented) - what to model next.
#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

struct ShaderUse
{
    std::string name, status, note, exampleA3, exampleMod;
    int a3 = 0, mods = 0;
};

class ShaderSurvey
{
public:
    void Start(const std::filesystem::path& projectRoot); // every *.rvmat under it (P:\ when empty)
    bool Poll();                                         // a finished scan taken over (results, text); true: new
    bool Busy() const { return job_.valid(); }
    std::vector<ShaderUse> results;                      // most used first
    std::string text;                                    // the table as text (copy, ui-script report)
    std::atomic<int> progress{ 0 };                      // files read

private:
    std::future<std::vector<ShaderUse>> job_;
};
