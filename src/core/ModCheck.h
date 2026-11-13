#pragma once
// "Mod check": every P3D / model.cfg / config.cpp / RVMAT / RTM below a mod folder is read and validated in the
// background (read only: nothing is fixed or written next to the mod). The result is a list of problems per file, an HTML
// report in %LOCALAPPDATA%\ArmaView\modcheck and the difference to the previous run of the same folder.
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

class ArmaConfig;

struct ModIssue
{
    enum class Sev { Error, Warning };
    Sev sev = Sev::Error;
    std::string kind;            // p3d, model.cfg, config.cpp, rvmat, rtm
    std::filesystem::path file;  // where it is (the source file of a config line when known)
    int line = 0;
    std::string text;
    bool isNew = false;          // not in the previous run of this folder
    std::string Key() const;     // identity across runs (file, kind, text without numbers in brackets)
};

struct ModReport
{
    std::filesystem::path folder;
    std::vector<ModIssue> issues;
    std::vector<ModIssue> fixed;     // in the previous run, not any more
    std::map<std::string, int> files; // kind -> files checked
    bool hadPrevious = false;
    std::string previousWhen, when;
    double seconds = 0;
    bool cancelled = false;
    std::filesystem::path html;      // the written report
    int Count(ModIssue::Sev s) const;
};

struct ModCheckProgress
{
    std::atomic<int> done{ 0 }, total{ 0 };
    std::atomic<bool> cancel{ false };
};

// Runs on any thread. game: the game's configs (base classes of config.cpp), may be null.
ModReport RunModCheck(const std::filesystem::path& folder, const std::filesystem::path& projectRoot, std::shared_ptr<const ArmaConfig> game,
                      ModCheckProgress& progress);
// Marks new issues and lists the fixed ones against the previous run of the folder, then stores this run and writes the
// HTML report (ModReport::html).
void FinishModReport(ModReport& r);
// A mod kept in a subfolder of P: names its files by its prefix ($PBOPREFIX$ or the folder name): when that prefix is
// not a folder of the project root, it is mounted (fallback) to the mod folder.
void MountModPrefix(const std::filesystem::path& modFolder, const std::filesystem::path& projectRoot);
std::filesystem::path ModCheckDir(); // %LOCALAPPDATA%\ArmaView\modcheck
