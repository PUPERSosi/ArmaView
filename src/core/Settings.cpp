#include "Settings.h"
#include "Log.h"
#include "Text.h"

#include <fstream>
#include <iterator>
#include <sstream>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {
constexpr const char* kHeader = "### ";

std::string ReadAll(const fs::path& p, bool& ok)
{
    std::ifstream f(p, std::ios::binary);
    ok = bool(f);
    return ok ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}
} // namespace

void Settings::ParseText(const std::string& text)
{
    blocks_.clear();
    std::istringstream is(text);
    std::string name, body;
    bool open = false;
    auto close = [&] {
        if (open) blocks_[name] = body;
        body.clear();
    };
    for (std::string line; std::getline(is, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with(kHeader))
        {
            close();
            name = line.substr(4);
            open = true;
            continue;
        }
        if (open) body += line + "\n";
    }
    close();
}

std::string Settings::Text() const
{
    std::string out = "# ArmaView settings (viewer only; blocks: ui, imgui, layout <name>, models)\n";
    for (const auto& [name, body] : blocks_)
    {
        out += kHeader + name + "\n" + body;
        if (!body.empty() && body.back() != '\n') out += "\n";
    }
    return out;
}

bool Settings::Load()
{
    bool ok = false;
    const std::string t = ReadAll(file_, ok);
    if (!ok)
    {
        blocks_.clear();
        return false;
    }
    ParseText(t);
    return true;
}

bool Settings::SaveTo(const fs::path& file) const
{
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string t = Text();
        f.write(t.data(), std::streamsize(t.size()));
        if (!f) return false;
    }
    fs::rename(tmp, file, ec); // replaces atomically on the same volume
    if (ec)
    {
        Log::Get().Warn("Settings: cannot write {}: {}", PathToUtf8(file), ec.message());
        return false;
    }
    return true;
}

bool Settings::Save() const { return !file_.empty() && SaveTo(file_); }

bool Settings::LoadFrom(const fs::path& file)
{
    bool ok = false;
    const std::string t = ReadAll(file, ok);
    if (!ok || t.find(kHeader) == std::string::npos) return false; // not a settings file
    ParseText(t);
    return true;
}

std::string Settings::Block(const std::string& block) const
{
    auto it = blocks_.find(block);
    return it == blocks_.end() ? std::string() : it->second;
}

void Settings::SetBlock(const std::string& block, std::string text) { blocks_[block] = std::move(text); }
void Settings::RemoveBlock(const std::string& block) { blocks_.erase(block); }

std::vector<std::string> Settings::BlocksWithPrefix(const std::string& prefix) const
{
    std::vector<std::string> out;
    for (const auto& [name, body] : blocks_)
        if (name.starts_with(prefix)) out.push_back(name.substr(prefix.size()));
    return out;
}

std::vector<std::pair<std::string, std::string>> Settings::Pairs(const std::string& block) const
{
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream is(Block(block));
    for (std::string line; std::getline(is, line);)
        if (const size_t eq = line.find('='); eq != std::string::npos) out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    return out;
}

void Settings::SetPairs(const std::vector<std::pair<std::string, std::string>>& pairs, const std::string& block)
{
    std::string body;
    for (const auto& [k, v] : pairs) body += k + "=" + v + "\n";
    SetBlock(block, body);
}

std::string Settings::Get(const std::string& key, const std::string& def, const std::string& block) const
{
    for (const auto& [k, v] : Pairs(block))
        if (k == key) return v;
    return def;
}

void Settings::Set(const std::string& key, const std::string& value, const std::string& block)
{
    auto pairs = Pairs(block);
    bool found = false;
    for (auto& [k, v] : pairs)
        if (k == key)
        {
            v = value;
            found = true;
        }
    if (!found) pairs.emplace_back(key, value);
    SetPairs(pairs, block);
}
