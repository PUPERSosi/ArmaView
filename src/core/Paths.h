#pragma once
// Where the viewer's own files live, behind an interface (the platform knows the exe folder and %LOCALAPPDATA%; tests
// give a temporary folder). Every front end gets it in its constructor; nothing in the core asks the OS directly.
#include <filesystem>

class IPaths
{
public:
    virtual ~IPaths() = default;
    virtual std::filesystem::path Exe() const = 0;  // the program's folder (shaders, fonts, built-in data)
    virtual std::filesystem::path Data() const = 0; // the viewer's writable files (settings, layouts, caches, log)

    std::filesystem::path Cache() const { return Data() / "cache"; }        // CfgMoves graphs, game configs
    std::filesystem::path Graphs() const { return Data() / "graphs"; }      // node positions of the graph windows
    std::filesystem::path Presets() const { return Data() / "retarget"; }   // bone mapping presets per skeleton pair
    std::filesystem::path Log() const { return Data() / "ArmaView.log"; }
};

// A fixed folder for both (tests, the command line tool).
class FixedPaths : public IPaths
{
public:
    explicit FixedPaths(std::filesystem::path dir) : dir_(std::move(dir)) {}
    std::filesystem::path Exe() const override { return dir_; }
    std::filesystem::path Data() const override { return dir_; }

private:
    std::filesystem::path dir_;
};
