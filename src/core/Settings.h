#pragma once
// The viewer's own settings in one text file (IPaths::Data() / settings.txt): named blocks of lines.
//   ### ui            key=value lines (UI scale, language, recording, start states ...)
//   ### imgui         the window / docking layout as ImGui writes it (every mode has its own dock space)
//   ### layout <name> a named layout (an ImGui layout snapshot)
//   ### models        hide / isolate / face sets per model (ModelVisibility)
// Written atomically (temp file + rename).
#include <filesystem>
#include <map>
#include <string>
#include <vector>

class Settings
{
public:
    explicit Settings(std::filesystem::path file = {}) : file_(std::move(file)) {}
    const std::filesystem::path& File() const { return file_; }
    void SetFile(std::filesystem::path file) { file_ = std::move(file); }

    bool Load();                          // false: no file (empty settings)
    bool Save() const;                    // atomic
    bool LoadFrom(const std::filesystem::path& file); // import (replaces everything)
    bool SaveTo(const std::filesystem::path& file) const; // export
    std::string Text() const;
    void ParseText(const std::string& text);

    bool Has(const std::string& block) const { return blocks_.count(block) > 0; }
    std::string Block(const std::string& block) const;
    void SetBlock(const std::string& block, std::string text);
    void RemoveBlock(const std::string& block);
    std::vector<std::string> BlocksWithPrefix(const std::string& prefix) const; // names after the prefix

    // key=value lines of a block (default "ui")
    std::string Get(const std::string& key, const std::string& def = {}, const std::string& block = "ui") const;
    void Set(const std::string& key, const std::string& value, const std::string& block = "ui");
    std::vector<std::pair<std::string, std::string>> Pairs(const std::string& block = "ui") const;
    void SetPairs(const std::vector<std::pair<std::string, std::string>>& pairs, const std::string& block = "ui");

private:
    std::filesystem::path file_;
    std::map<std::string, std::string> blocks_;
};

