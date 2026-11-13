// Machine-specific paths (Arma 3 Samples, test models, Blender) read from armaview.local.ini - not in the repository.
// Looked up next to the exe first, then in %LOCALAPPDATA%\ArmaView (the setup window saves there), then in the source tree (see armaview.local.ini.example there).
#pragma once
#include <filesystem>
#include <string>

struct LocalPaths
{
    std::string samples;      // ...\Arma 3 Samples\Addons
    std::string blender;      // blender.exe
    std::filesystem::path file; // where it was read from (empty: not found)

    std::string SampleMan() const; // Test_Character_01\A3_character_example.p3d under samples (empty if samples unset)
};

const LocalPaths& GetLocalPaths();
// Sets key = value in an armaview.local.ini (created if missing, other lines kept). Read at the next start.
bool SaveLocalPath(const std::filesystem::path& iniFile, const std::string& key, const std::string& value);
// The Arma path mounts every front end uses: \Samples_f\... -> the samples folder (BI's samples reference themselves so),
// \a3\... -> P:\a3 also for models opened from elsewhere (the game data stays on P:).
void AddStandardMounts(const LocalPaths& lp);
