#pragma once
// Arma config (model.cfg / config.cpp / rvmat) reader: basic preprocessor + class tree with inheritance.
// Supported: // and /* */ comments, #include, #define (object and function-like, # and ##), #undef,
// #ifdef/#ifndef/#else/#endif, class A : B {}, class A; (external), delete A;, x = v;, x[] = {...}; x[] += {...};
// Errors go to the Log as "file(line): message"; parsing continues with the next statement.
#include "FileSystem.h"

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct CfgValue
{
    enum class Type { String, Number, Array };
    Type type = Type::String;
    std::string str; // string content, or source text of a number
    double num = 0.0;
    std::vector<CfgValue> items;
};

struct CfgEntry
{
    std::string name;
    CfgValue value;
    bool isArray = false;
    int file = 0, line = 0;
};

class CfgClass
{
public:
    std::string name, baseName;
    bool external = false; // "class X;" — refers to an inherited class
    int file = 0, line = 0;
    CfgClass* parent = nullptr;
    CfgClass* base = nullptr; // resolved base (or the target of an external declaration)
    // Same class in the base (game) configs this addon config is overlaid on: searched after the own inheritance
    // chain, like the game merges an addon's class into its existing one.
    const CfgClass* overlay = nullptr;
    // The inheritance chain including overlays, nearest first (this, bases, then the overlays of each).
    std::vector<const CfgClass*> Chain() const;
    std::vector<CfgEntry> entries;
    std::vector<std::unique_ptr<CfgClass>> classes;

    const CfgEntry* Find(std::string_view entry, bool inherited = true) const;
    const CfgClass* FindClass(std::string_view cls, bool inherited = true) const;

    std::string GetString(std::string_view entry, std::string_view def = {}) const;
    double GetNumber(std::string_view entry, double def = 0.0) const; // evaluates expressions ("rad 45")
    bool HasNumber(std::string_view entry) const;
    std::vector<std::string> GetStrings(std::string_view entry) const; // array flattened to strings
};

class ArmaConfig
{
public:
    // projectRoot resolves absolute includes like "\MyMod\file.hpp" (normally P:\).
    // reportMissingBases=false: addon configs (config.cpp) inherit from game classes that are not on disk;
    // such bases are counted in MissingBases() instead of being logged as errors.
    // Text (preprocessed) or rapified binary ("\0raP") config.
    bool Load(const std::filesystem::path& path, const std::filesystem::path& projectRoot, bool reportMissingBases = true);
    // Several configs merged into one tree in the given order (later files extend / override earlier classes, like
    // addons in the game). keepRoots (lower case; empty = all): other root classes are dropped after each file.
    // Errors are counted, not logged (game data).
    bool LoadMerged(const std::vector<std::filesystem::path>& files, const std::filesystem::path& projectRoot,
                    const std::vector<std::string>& keepRoots);
    // Base configs this one is overlaid on (the game's): a class that also exists there at the same path extends that
    // class, and base names not found here are looked up there. Set before Load.
    void SetFallback(std::shared_ptr<const ArmaConfig> fallback) { fallback_ = std::move(fallback); }
    const ArmaConfig* Fallback() const { return fallback_.get(); }
    int MissingBases() const { return missingBases_; }
    // Binary snapshot of the parsed tree (game configs: skips ~5 s of parsing). key identifies the source files.
    // The key is computed from the file list itself (sizes / times of those files + the folders they are in), so a
    // warm start does not walk the whole game data tree.
    bool SaveCache(const std::filesystem::path& file) const;
    bool LoadCache(const std::filesystem::path& file);

    const CfgClass& Root() const { return root_; }
    std::string Where(int file, int line) const;
    // File of a class / entry index; indices of the fallback (game) configs resolve there.
    std::filesystem::path FileOf(int file) const;
    const std::vector<std::filesystem::path>& Files() const { return files_; }
    int Errors() const { return errors_; }

    // Internal: used by the preprocessor/parser.
    int AddFile(const std::filesystem::path& p);
    void Error(int file, int line, const std::string& msg);
    void Warn(int file, int line, const std::string& msg);

private:
    CfgClass root_;
    std::vector<std::filesystem::path> files_;
    int errors_ = 0;
    int missingBases_ = 0;
    bool reportMissingBases_ = true;
    bool quiet_ = false;
    int fileBase_ = 0; // game configs number their files from 1<<20 so overlays can tell them apart
    std::shared_ptr<const ArmaConfig> fallback_;
    friend void ResolveBasesFor(ArmaConfig& cfg, CfgClass& cls, const CfgClass* counterpart);
};

// The game's configs unpacked on the project drive, in load order: <root>\bin\config.bin (rapified core), then every
// <root>\a3\**\config.cpp ordered by CfgPatches requiredAddons. Empty if there is no <root>\a3 folder.
std::vector<std::filesystem::path> GameConfigFiles(const std::filesystem::path& projectRoot);

// Deep copy (base pointers are not copied: resolve again); merge of src into dst like a later addon.
std::unique_ptr<CfgClass> CloneClass(const CfgClass& src, CfgClass* parent);
void MergeClass(CfgClass& dst, CfgClass&& src);

// Child classes of cls merged along its inheritance chain (most derived definition of each name first);
// an external declaration stands for the class it resolves to (skipped if unresolved).
std::vector<const CfgClass*> MergedChildClasses(const CfgClass& cls);

// Fingerprint of config sources: each file's path, size and time plus the times of the folders above them (a new
// addon folder changes its parent's time).
uint64_t ConfigFilesStamp(const std::vector<std::filesystem::path>& files);

// True if the file starts with the rapified signature "\0raP".
bool IsRapified(const std::filesystem::path& path);
// Reads a rapified config into `root` (merging into existing classes). Errors go to cfg.Error.
bool LoadRap(ArmaConfig& cfg, const std::filesystem::path& path, CfgClass& root);

// Evaluates simple numeric config expressions as used in model.cfg: numbers, + - * /, parentheses,
// unary minus, "rad x", "deg x", "pi" (e.g. "rad -45", "(rad 90)", "rad(10)", "-rad 5"). False if not numeric.
bool EvalNumber(std::string_view expr, double& out);

// File ids: a config.cpp and its includes count from 0, the merged game configs (SetFallback / LoadMerged) from here.
constexpr int kGameFileBase = 1 << 20;

// The Arma project root ("P:\") for a path: P:\ itself, the folder P: is SUBSTed to, or the drive root.
std::filesystem::path ArmaProjectRoot(const std::filesystem::path& anyPath);
// What a drive letter is mapped to (SUBST: "\??\C:\Work"), installed by the platform layer; without it only P:\ itself.
using DriveTargetFn = std::wstring (*)(wchar_t drive);
void SetDriveTargetResolver(DriveTargetFn fn);

// Arma paths ("\MyMod\x.p3d" or "MyMod\x.p3d") -> files: the project root plus virtual mounts.
//  - mounts: data outside the project drive, e.g. "Samples_f" -> ...\Arma 3 Samples\Addons (BI's samples reference
//    themselves as \Samples_f\...). Apply to Resolve and PathOf.
//  - fallback mounts: a mod's own prefix ("\MyMod\..." of a mod kept in P:\MyMods\MyMod), used by Resolve only when the
//    path does not exist under the project root; PathOf names a file under the mod's folder by the prefix (the paths
//    its config uses: model= "\AL_Mods\mod\x.p3d" for P:\Modding\mod\x.p3d).
// Thread safe (background loads resolve while the UI adds a mount).
class ArmaPathResolver
{
public:
    explicit ArmaPathResolver(const IFileSystem& fs);
    void AddMount(std::string_view prefix, const std::filesystem::path& folder);
    void AddFallbackMount(std::string_view prefix, const std::filesystem::path& folder);
    std::filesystem::path Resolve(std::string_view armaPath, const std::filesystem::path& projectRoot) const;
    // Arma path of a file on disk ("Samples_f\Test_Tank_01\Test_Tank_01.p3d" or relative to projectRoot)
    std::string PathOf(const std::filesystem::path& file, const std::filesystem::path& projectRoot) const;

private:
    const IFileSystem& fs_;
    mutable std::shared_mutex mutex_;
    std::vector<std::pair<std::string, std::filesystem::path>> mounts_, fallback_;
};

// TODO(migration step 12): the process resolver behind the free functions below; owned by the Session instead later.
ArmaPathResolver& DefaultArmaPaths();
std::filesystem::path ResolveArmaPath(std::string_view armaPath, const std::filesystem::path& projectRoot);
void AddArmaMount(const std::string& prefix, const std::filesystem::path& folder);
void AddArmaFallbackMount(const std::string& prefix, const std::filesystem::path& folder);
std::string ArmaPathOf(const std::filesystem::path& file, const std::filesystem::path& projectRoot);

// The Arma path prefix of a mod folder (what its files are called in the game: "AL_Mods\am_pin_tigr" for
// P:\Modding\am_pin_tigr): $PBOPREFIX$; else the prefix set by hand for that folder (SetModPrefixOverrides, from the
// settings); else the one the parsed config's own paths agree on (for "\a\b\rest" the longest tail "rest" that exists
// under the folder, the head "a\b" by majority); else the folder name. why: how it was found.
std::string InferModPrefix(const std::filesystem::path& modFolder, const ArmaConfig* parsed, std::string* why = nullptr);
// Mod folder -> prefix set by the user (settings "modprefix" block); replaces the previous list.
void SetModPrefixOverrides(std::vector<std::pair<std::filesystem::path, std::string>> folderToPrefix);
// The mod's prefix inferred and mounted when the folder is not at that prefix on the project drive (fallback mount).
// Returns the prefix ("" none).
std::string MountInferredModPrefix(const std::filesystem::path& modFolder, const ArmaConfig* parsed, const std::filesystem::path& projectRoot);
