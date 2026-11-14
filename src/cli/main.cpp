// armaview-cli: the core without a window.
//   armaview-cli validate <mod folder> [--no-game]   Mod check (configs, models, RTMs, textures); exit 1 on errors
//   armaview-cli info <file.p3d|.gltf|.glb|.rtm>        LODs, skeleton, model.cfg animations, model checks
//   armaview-cli selftest [--fuzz N]                   core self test + parser fuzzing on its files; exit 1 on failures
// Log: warnings and errors to stderr, -v also info.
#include "Fuzz.h"
#include "LocalPaths.h"
#include "Log.h"
#include "ModCheck.h"
#include "RTM.h"
#include "SelfTest.h"
#include "Strings.h"
#include "Session.h"
#include "Validation.h"
#include "ViewCoreTest.h"
#include "WinFileSystem.h"
#include "WinPlatform.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class ConsoleSink : public ILogSink
{
public:
    explicit ConsoleSink(bool verbose) : verbose_(verbose) {}
    void Write(LogLevel level, const std::string& text) override
    {
        if (level == LogLevel::Debug || (level == LogLevel::Info && !verbose_)) return;
        static constexpr const char* tags[] = { "dbg", "inf", "WRN", "ERR" };
        std::fprintf(stderr, "[%s] %s\n", tags[int(level)], text.c_str());
    }

private:
    bool verbose_;
};

fs::path ExeFolder()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

int Usage()
{
    std::puts("armaview-cli validate <mod folder> [--no-game] [-v]\n"
              "armaview-cli info <file.p3d|file.gltf|file.glb|file.rtm> [-v]\n"
              "armaview-cli selftest [--fuzz N] [-v]");
    return 2;
}

int Validate(const fs::path& folder, bool withGame)
{
    std::shared_ptr<const ArmaConfig> game;
    if (withGame) game = LoadGameConfigs("P:\\", ExeFolder() / "cache" / "gameconfigs.bin");
    ModCheckProgress progress;
    const fs::path root = ArmaProjectRoot(folder);
    MountModPrefix(folder, root); // as the viewer's Mod check: the mod's own paths when it is not at its prefix on P:
    ModReport r = RunModCheck(folder, root, game, progress);
    FinishModReport(r); // HTML report + comparison with the previous run of this folder
    const int errors = r.Count(ModIssue::Sev::Error), warnings = r.Count(ModIssue::Sev::Warning);
    for (const auto& i : r.issues)
        std::printf("%s %s %s%s: %s\n", i.sev == ModIssue::Sev::Error ? "ERROR  " : "warning", i.kind.c_str(), PathToUtf8(i.file).c_str(),
                    i.line ? std::format("({})", i.line).c_str() : "", i.text.c_str());
    std::string files;
    for (const auto& [k, n] : r.files) files += std::format(" {} {}", k, n);
    std::printf("%s: %d error(s), %d warning(s); files%s%s\n", PathToUtf8(r.folder).c_str(), errors, warnings, files.c_str(),
                game ? "" : " (without the game configs)");
    if (!r.html.empty()) std::printf("report: %s\n", PathToUtf8(r.html).c_str());
    return errors ? 1 : 0;
}

int Info(const fs::path& file)
{
    const std::string ext = ToLower(PathToUtf8(file.extension()));
    if (ext == ".rtm")
    {
        RtmAnimation rtm;
        std::string err;
        std::vector<std::string> warnings;
        if (!LoadRtm(file, rtm, err, warnings))
        {
            std::fprintf(stderr, "%s: %s\n", PathToUtf8(file).c_str(), err.c_str());
            return 1;
        }
        for (const auto& w : warnings) std::printf("warning: %s\n", w.c_str());
        std::printf("%s: %zu frame(s), %zu bone(s)\n", PathToUtf8(file).c_str(), rtm.Frames(), rtm.Bones());
        return 0;
    }
    WinFileSystem disk;
    EventBus events;
    Session s(disk, events);
    P3DModel m;
    if (!Session::ReadModel(file, false, m)) return 1;
    s.SetModel(std::move(m), false);
    s.LoadAddonConfig();
    std::printf("%s: %zu LOD(s)\n", PathToUtf8(file).c_str(), s.model.lods.size());
    for (size_t i = 0; i < s.model.lods.size(); ++i)
    {
        const P3DLod& l = s.model.lods[i];
        std::printf("  LOD %zu [%s]: points %zu, faces %zu, selections %zu\n", i, l.Name().c_str(), l.points.size(), l.faces.size(), l.selections.size());
    }
    std::printf("skeleton: %s (%zu bones), model.cfg animations %zu\n", s.skeleton.name.empty() ? "-" : s.skeleton.name.c_str(),
                s.skeleton.bones.size(), s.cfgAnims.size());
    ValidationReport v;
    std::vector<int> boneSelection(s.skeleton.bones.size(), -1);
    if (!s.skeleton.Empty() && !s.model.lods.empty())
    {
        v = Validate(s.skeleton, s.model.lods[0], s.modelInfo.sections);
        const P3DLod& l0 = s.model.lods[0];
        for (size_t b = 0; b < s.skeleton.bones.size(); ++b)
            for (size_t k = 0; k < l0.selections.size(); ++k)
                if (IEquals(l0.selections[k].name, s.skeleton.bones[b].name)) { boneSelection[b] = int(k); break; }
    }
    const ModelChecks c = CheckModel(s, v, boneSelection);
    std::printf("%sRESULT: %d error(s), %d warning(s)\n", c.text.c_str(), c.errors, c.warnings);
    return c.errors || Log::Get().Errors() ? 1 : 0;
}

int SelfTest(int fuzz)
{
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "armaview_selftest_core";
    int failures = 0;
    std::printf("coretest: %s\n", RunCoreTest(dir, &failures).c_str());
    int viewFailures = 0; // the view logic another UI builds on (input -> scene interaction, overlay settings)
    std::printf("view coretest: %s\n", RunViewCoreTest(fs::temp_directory_path(ec) / "armaview_selftest_view", &viewFailures).c_str());
    failures += viewFailures;
    if (fuzz > 0)
    {
        std::vector<fs::path> seeds;
        for (const auto& e : fs::directory_iterator(dir, ec)) seeds.push_back(e.path());
        int exceptions = 0;
        std::printf("fuzz: %s\n", FuzzRun(seeds, fuzz, 12345u, &exceptions).c_str());
        failures += exceptions;
    }
    return failures || Log::Get().Errors() ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    InstallWinPlatform();
    SetUiLang(UiLang::En); // console output in English
    AddStandardMounts(GetLocalPaths()); // as the viewer: \Samples_f\..., \a3\... on P:
    std::vector<std::wstring> args(argv + 1, argv + argc);
    bool verbose = false, withGame = true;
    int fuzz = 100;
    std::vector<std::wstring> pos;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == L"-v") verbose = true;
        else if (args[i] == L"--no-game") withGame = false;
        else if (args[i] == L"--fuzz" && i + 1 < args.size()) fuzz = _wtoi(args[++i].c_str());
        else pos.push_back(args[i]);
    }
    ConsoleSink sink(verbose);
    Log::Get().AddSink(&sink);
    int rc = 2;
    if (pos.size() == 2 && pos[0] == L"validate") rc = Validate(pos[1], withGame);
    else if (pos.size() == 2 && pos[0] == L"info") rc = Info(pos[1]);
    else if (pos.size() == 1 && pos[0] == L"selftest") rc = SelfTest(fuzz);
    else rc = Usage();
    Log::Get().RemoveSink(&sink);
    return rc;
}
