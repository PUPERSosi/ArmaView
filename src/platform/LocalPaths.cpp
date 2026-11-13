#include "LocalPaths.h"
#include "ArmaConfig.h"

#include <fstream>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

namespace
{
    std::string Trim(std::string s)
    {
        const size_t b = s.find_first_not_of(" \t\r\"");
        if (b == std::string::npos) return {};
        const size_t e = s.find_last_not_of(" \t\r\"");
        return s.substr(b, e - b + 1);
    }

    fs::path ExeFolder()
    {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path();
    }

    LocalPaths Load()
    {
        LocalPaths p;
        // ARMAVIEW_LOCAL_INI: another file, or "none" (a run without machine paths, e.g. the core selftest of CI)
        wchar_t env[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ARMAVIEW_LOCAL_INI", env, MAX_PATH) > 0)
        {
            if (_wcsicmp(env, L"none") == 0) return p;
        }
        std::vector<fs::path> candidates{ ExeFolder() / "armaview.local.ini" };
        if (wchar_t app[MAX_PATH] = {}; GetEnvironmentVariableW(L"LOCALAPPDATA", app, MAX_PATH) > 0)
            candidates.push_back(fs::path(app) / L"ArmaView" / "armaview.local.ini");
        if (env[0]) candidates.insert(candidates.begin(), fs::path(env));
#ifdef ARMAVIEW_SOURCE_DIR
        candidates.push_back(fs::path(ARMAVIEW_SOURCE_DIR) / "armaview.local.ini");
#endif
        for (const auto& c : candidates)
        {
            std::error_code ec;
            if (!fs::exists(c, ec)) continue;
            std::ifstream f(c);
            for (std::string line; std::getline(f, line);)
            {
                line = Trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#') continue;
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
                if (k == "samples") p.samples = v;
                else if (k == "blender") p.blender = v;
            }
            p.file = c;
            break;
        }
        return p;
    }
}

std::string LocalPaths::SampleMan() const
{
    if (samples.empty()) return {};
    return (fs::path(samples) / "Test_Character_01" / "A3_character_example.p3d").string();
}

const LocalPaths& GetLocalPaths()
{
    static const LocalPaths p = Load();
    return p;
}

void AddStandardMounts(const LocalPaths& lp)
{
    std::error_code ec;
    if (!lp.samples.empty() && std::filesystem::is_directory(lp.samples, ec)) AddArmaMount("Samples_f", lp.samples);
    if (std::filesystem::is_directory("P:\\a3", ec)) AddArmaMount("a3", "P:\\a3");
}

bool SaveLocalPath(const fs::path& iniFile, const std::string& key, const std::string& value)
{
    std::vector<std::string> lines;
    bool set = false;
    if (std::ifstream f(iniFile); f)
        for (std::string line; std::getline(f, line);)
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq != std::string::npos && line[0] != ';' && line[0] != '#' && Trim(line.substr(0, eq)) == key)
            {
                line = key + " = " + value;
                set = true;
            }
            lines.push_back(line);
        }
    if (!set) lines.push_back(key + " = " + value);
    std::error_code ec;
    fs::create_directories(iniFile.parent_path(), ec);
    std::ofstream o(iniFile, std::ios::trunc);
    for (const auto& l : lines) o << l << '\n';
    return bool(o);
}
